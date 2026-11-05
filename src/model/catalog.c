#include "denoise.h"
#include "cJSON.h"
#include "catalog_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include <ctype.h>

#define MAX_MODELS 128
#define CATALOG_URL L"https://raw.githubusercontent.com/Artur73737/denoise/main/catalog/models.json"
static DnModel models[MAX_MODELS];
static size_t model_count;
static cJSON *catalog_root;
static char status_text[256] = "Bundled catalog";

static const char *text_field(const cJSON *object, const char *key, size_t maximum) {
    const cJSON *item=cJSON_GetObjectItemCaseSensitive(object,key);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0] || strlen(item->valuestring)>maximum) return NULL;
    return item->valuestring;
}
static int number_field(const cJSON *object, const char *key, double maximum, uint64_t *result) {
    const cJSON *item=cJSON_GetObjectItemCaseSensitive(object,key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble<0 || item->valuedouble>maximum || floor(item->valuedouble)!=item->valuedouble) return 0;
    *result=(uint64_t)item->valuedouble; return 1;
}
static int filename_ok(const char *name) {
    const unsigned char *p=(const unsigned char *)name;
    char stem[128]; size_t n;
    if (!name || !name[0] || name[0]=='.' || name[strlen(name)-1]=='.' || strstr(name,"..")) return 0;
    for (;*p;++p) if (!((*p>='a'&&*p<='z') || (*p>='A'&&*p<='Z') || (*p>='0'&&*p<='9') || *p=='_' || *p=='-' || *p=='.')) return 0;
    n=strcspn(name,".");if(n>=sizeof(stem))return 0;
    memcpy(stem,name,n);stem[n]=0;
    if (!_stricmp(stem,"CON") || !_stricmp(stem,"PRN") || !_stricmp(stem,"AUX") || !_stricmp(stem,"NUL") ||
        (n==4 && (!_strnicmp(stem,"COM",3) || !_strnicmp(stem,"LPT",3)) && stem[3]>='1' && stem[3]<='9')) return 0;
    return 1;
}
static int hash_ok(const char *hash) {
    size_t i;
    if (!hash || strlen(hash)!=64) return 0;
    for (i=0;i<64;++i) if (!((hash[i]>='0'&&hash[i]<='9') || (hash[i]>='a'&&hash[i]<='f'))) return 0;
    return 1;
}
static int url_ok(const char *url) {
    const unsigned char *p=(const unsigned char *)url;
    if (!url || (strncmp(url,"https://huggingface.co/",23) && strncmp(url,"https://raw.githubusercontent.com/",34) && strncmp(url,"https://github.com/",19))) return 0;
    for (;*p;++p) if (*p<=32 || *p>=127 || *p=='\\') return 0;
    return 1;
}
static int unambiguous(const cJSON *node,int depth) {
    const cJSON *a,*b;int count=0;
    if(depth>16)return 0;
    for(a=node->child;a;a=a->next) {
        if(++count>(cJSON_IsObject(node)?64:128))return 0;
        if(cJSON_IsObject(node))for(b=node->child;b!=a;b=b->next)if(a->string && b->string && !strcmp(a->string,b->string))return 0;
        if(a->child && !unambiguous(a,depth+1))return 0;
    }
    return 1;
}
static int parse_catalog(const char *json, size_t length, cJSON **root_out, DnModel next[MAX_MODELS], size_t *count_out, DnError *err) {
    cJSON *root=NULL;
    const cJSON *array, *item;
    uint64_t value;
    int n, index=0;
    size_t i,j;
    const char *required[]={"scunet_color_15","scunet_color_25","scunet_color_50","scunet_gray_15","scunet_gray_25","scunet_gray_50","scunet_color_real_psnr"};
    if (!json || !length || length>512*1024 || memchr(json,0,length)) return dn_error(err,"Invalid catalog length or embedded NUL");
    {
        const char *end=NULL;
        root=cJSON_ParseWithLengthOpts(json,length,&end,0);
        if (!root || !end) goto invalid;
        while (end<json+length && isspace((unsigned char)*end)) ++end;
        if (end!=json+length) goto invalid;
    }
    if (!root || !cJSON_IsObject(root) || !unambiguous(root,0) || !number_field(root,"schema",1,&value) || value!=1 || !text_field(root,"version",80)) goto invalid;
    array=cJSON_GetObjectItemCaseSensitive(root,"models");
    if (!cJSON_IsArray(array) || (n=cJSON_GetArraySize(array))<7 || n>MAX_MODELS) goto invalid;
    memset(next,0,sizeof(DnModel)*MAX_MODELS);
    cJSON_ArrayForEach(item,array) {
        DnModel *m=&next[index++];
        const cJSON *files, *file;
        const char *adapter;
        int f=0, file_count;
        m->id=text_field(item,"id",100); m->title=text_field(item,"title",180);
        m->description=text_field(item,"description",1200); m->performance=text_field(item,"performance",1200);
        m->license=text_field(item,"license",200); m->source=text_field(item,"source",1024);
        adapter=text_field(item,"adapter",32);
        if (!filename_ok(m->id) || !m->title || !m->description || !m->performance || !m->license || !m->source || strncmp(m->source,"https://",8) || !adapter) goto invalid;
        if (!number_field(item,"channels",3,&value) || (value!=1 && value!=3)) goto invalid;
        m->channels=(int)value;
        if (!number_field(item,"input_channels",4,&value)) goto invalid;
        m->input_channels=(int)value;
        if (!strcmp(adapter,"image")) { if (m->input_channels!=m->channels) goto invalid; }
        else if (!strcmp(adapter,"noise-map")) { if (m->input_channels!=4 || m->channels!=3) goto invalid; m->noise_map=1; }
        else goto invalid;
        if (!number_field(item,"fixed_tile",512,&value) || (value && (value<128 || value%64))) goto invalid;
        m->fixed_tile=(int)value;
        files=cJSON_GetObjectItemCaseSensitive(item,"files");
        if (!cJSON_IsArray(files) || (file_count=cJSON_GetArraySize(files))<1 || file_count>2) goto invalid;
        cJSON_ArrayForEach(file,files) {
            const char *name=text_field(file,"name",120), *url=text_field(file,"url",2047), *hash=text_field(file,"sha256",64);
            if (!filename_ok(name) || !url_ok(url) || !hash_ok(hash) || !number_field(file,"bytes",8.0*1024*1024*1024,&value) || !value) goto invalid;
            if (!f) {
                size_t len=strlen(name);
                if (len<6 || strcmp(name+len-5,".onnx")) goto invalid;
                m->graph_file=name; m->graph_url=url; m->graph_hash=hash; m->graph_bytes=(size_t)value;
            } else {
                if (!strcmp(name,m->graph_file)) goto invalid;
                m->weights_file=name; m->weights_url=url; m->weights_hash=hash; m->weights_bytes=(size_t)value;
            }
            ++f;
        }
    }
    for (i=0;i<(size_t)n;++i) for (j=0;j<i;++j) {
        int a,b;
        if (!_stricmp(next[i].id,next[j].id)) goto invalid;
        for (a=0;a<2;++a) for (b=0;b<2;++b) {
            const char *left=a?next[i].weights_file:next[i].graph_file,*right=b?next[j].weights_file:next[j].graph_file;
            if (left && right && !_stricmp(left,right)) goto invalid;
        }
    }
    for (i=0;i<sizeof(required)/sizeof(required[0]);++i) {
        for (j=0;j<(size_t)n && strcmp(required[i],next[j].id);++j) {}
        if (j==(size_t)n) goto invalid;
    }
    *root_out=root; *count_out=(size_t)n; return 1;
invalid:
    if (root) cJSON_Delete(root);
    return dn_error(err,"Catalog rejected: invalid schema, model adapter, URL, file name, size, hash or duplicate entry");
}
int dn_catalog_validate_json(const char *json, size_t length, DnError *err) {
    DnModel next[MAX_MODELS]; cJSON *root=NULL; size_t count;
    int ok=parse_catalog(json,length,&root,next,&count,err);
    if (root) cJSON_Delete(root);
    return ok;
}
static int apply_catalog(const char *json,size_t length,const char *source,DnError *err) {
    DnModel next[MAX_MODELS]; cJSON *root=NULL; size_t count;
    if (!parse_catalog(json,length,&root,next,&count,err)) return 0;
    if (catalog_root) cJSON_Delete(catalog_root);
    catalog_root=root; model_count=count; memcpy(models,next,count*sizeof(DnModel));
    snprintf(status_text,sizeof(status_text),"%s | %zu models",source,count); return 1;
}
static int cache_path(wchar_t *directory,wchar_t *file,DnError *err) {
    return dn_models_directory(directory,err) && dn_path_join(file,DN_PATH,directory,L"catalog.json",err);
}
int dn_catalog_init(DnError *err) {
    wchar_t *dir=NULL,*path=NULL;
    FILE *file=NULL;
    char *json=NULL;
    long size;
    DnError ignored={{0}};
    if (model_count) return 1;
    if (!apply_catalog(dn_bundled_catalog,strlen(dn_bundled_catalog),"Bundled catalog",err)) return 0;
    dir=calloc(DN_PATH,sizeof(wchar_t));path=calloc(DN_PATH,sizeof(wchar_t));
    if (!dir || !path || !cache_path(dir,path,&ignored) || _wfopen_s(&file,path,L"rb") || !file) goto done;
    if (fseek(file,0,SEEK_END) || (size=ftell(file))<=0 || size>512*1024 || fseek(file,0,SEEK_SET)) goto done;
    json=malloc((size_t)size+1);
    if (!json || fread(json,1,(size_t)size,file)!=(size_t)size) goto done;
    json[size]=0; apply_catalog(json,(size_t)size,"Cached catalog",&ignored);
done:
    if (file) fclose(file);
    free(json);free(dir);free(path); return 1;
}
const char *dn_catalog_status(void) { DnError ignored={{0}};dn_catalog_init(&ignored);return status_text; }
int dn_catalog_refresh(void *context,DnError *err) {
    wchar_t *dir=calloc(DN_PATH,sizeof(wchar_t)),*path=calloc(DN_PATH,sizeof(wchar_t)),*temp=calloc(DN_PATH,sizeof(wchar_t));
    char *json=NULL; size_t length=0;
    HANDLE file=INVALID_HANDLE_VALUE; DWORD written;
    int reserved=0,ok=0;
    (void)context;
    if (!dn_catalog_init(err)) goto done;
    if (!dir || !path || !temp) { dn_error(err,"Out of memory"); goto done; }
    if (!dn_http_catalog(CATALOG_URL,&json,&length,err)) goto done;
    if (!dn_catalog_validate_json(json,length,err)) goto done;
    if (!cache_path(dir,path,err) || !dn_mkdir(dir,err)) goto done;
    if (swprintf_s(temp,DN_PATH,L"%ls.part-%lu-%llu",path,GetCurrentProcessId(),GetTickCount64())<0) { dn_error(err,"Catalog path is too long");goto done; }
    file=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) { dn_error(err,"Cannot create catalog cache (Windows error %lu)",GetLastError());goto done; }
    reserved=1;
    if (!WriteFile(file,json,(DWORD)length,&written,NULL) || written!=length || !FlushFileBuffers(file)) { dn_error(err,"Cannot write catalog cache");goto done; }
    CloseHandle(file);file=INVALID_HANDLE_VALUE;
    if (dn_cancelled()) { dn_error(err,"Catalog refresh canceled");goto done; }
    if (!MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { dn_error(err,"Cannot replace catalog cache");goto done; }
    if (!apply_catalog(json,length,"Online catalog",err)) goto done;
    dn_status(DN_DONE,"Fetched %zu models from the online catalog",model_count);ok=1;
done:
    if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    if (reserved && !ok) DeleteFileW(temp);
    if (!ok && model_count) {
        char previous[160];strncpy_s(previous,sizeof(previous),status_text,_TRUNCATE);
        snprintf(status_text,sizeof(status_text),"Offline fallback: %.150s",previous);
    }
    free(json);free(dir);free(path);free(temp);return ok;
}
size_t dn_model_count(void) { DnError ignored={{0}};dn_catalog_init(&ignored);return model_count; }
const DnModel *dn_model_at(size_t index) { return index<dn_model_count()?&models[index]:NULL; }
const DnModel *dn_model_find(const char *id) {
    char name[128]; size_t n,i;
    if (!id || strlen(id)>=sizeof(name)) return NULL;
    strcpy_s(name,sizeof(name),id);n=strlen(name);
    for (i=0;i<dn_model_count();++i) if (!_stricmp(name,models[i].id) || !_stricmp(name,models[i].graph_file)) return &models[i];
    if (n>5 && !_stricmp(name+n-5,".onnx")) name[n-5]=0;
    for (i=0;i<dn_model_count();++i) if (!_stricmp(name,models[i].id)) return &models[i];
    return NULL;
}
uint64_t dn_model_size(const DnModel *model) { return (uint64_t)model->graph_bytes+model->weights_bytes; }
int dn_model_ready(const DnModel *model,const wchar_t *cache) {
    wchar_t *path=calloc(DN_PATH,sizeof(wchar_t)); wchar_t file[128]; DnError err={{0}}; int i,ok=1;
    if (!path) return 0;
    for (i=0;i<(model->weights_bytes?2:1);++i) {
        swprintf_s(file,128,L"%hs",i?model->weights_file:model->graph_file);
        if (!dn_path_join(path,DN_PATH,cache,file,&err) || !dn_file_verify(path,i?model->weights_hash:model->graph_hash,i?model->weights_bytes:model->graph_bytes)) { ok=0;break; }
    }
    free(path);return ok;
}
const DnModel *dn_model_select(int grayscale,int amount,const char *override) {
    char id[80];const DnModel *explicit_model=dn_model_find(override);
    int sigma=amount<=33?15:amount<=66?25:50;
    if (explicit_model) return explicit_model;
    if (!strcmp(override,"real")) return dn_model_find("scunet_color_real_psnr");
    if (!strcmp(override,"15")) sigma=15;
    else if (!strcmp(override,"25")) sigma=25;
    else if (!strcmp(override,"50")) sigma=50;
    else if (strcmp(override,"auto")) return NULL;
    snprintf(id,sizeof(id),"scunet_%s_%d",grayscale?"gray":"color",sigma);return dn_model_find(id);
}
int dn_model_ensure(const DnModel *model,const wchar_t *cache,int offline,wchar_t *graph,DnError *err) {
    wchar_t *path=calloc(DN_PATH,sizeof(wchar_t));wchar_t filename[128],url[2048];
    int i,ok=0,count=model->weights_bytes?2:1;
    if (!path) return dn_error(err,"Out of memory");
    if (!dn_mkdir(cache,err)) goto done;
    for (i=0;i<count;++i) {
        const char *hash=i?model->weights_hash:model->graph_hash,*address=i?model->weights_url:model->graph_url;
        size_t bytes=i?model->weights_bytes:model->graph_bytes;
        swprintf_s(filename,128,L"%hs",i?model->weights_file:model->graph_file);
        if (!dn_path_join(path,DN_PATH,cache,filename,err)) goto done;
        dn_progress(DN_VERIFY,i,count,model->id,i?"Checking weight file (SHA-256)":"Checking model graph (SHA-256)");
        if (dn_cancelled()) { dn_error(err,"Canceled");goto done; }
        if (!dn_file_verify(path,hash,bytes)) {
            if (offline) { dn_error(err,"Model %s is missing or incomplete. Offline mode prevents download. Run 'denoise.exe model' to install it.",model->id);goto done; }
            if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,address,-1,url,2048)) { dn_error(err,"Invalid model URL encoding");goto done; }
            if (!dn_download(url,path,hash,bytes,err)) goto done;
        }
        if (!i) wcscpy_s(graph,DN_PATH,path);
    }
    dn_progress(DN_VERIFY,count,count,model->id,"Model verified and ready");ok=1;
done:
    free(path);return ok;
}
