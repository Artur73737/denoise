#include "denoise.h"
#include <stdio.h>
#include <stdlib.h>
int wmain(int argc,wchar_t **argv) {
    FILE *file=NULL;long length;char *json;size_t read;int ok;DnError err={{0}};
    if(argc!=2 || _wfopen_s(&file,argv[1],L"rb") || !file)return 2;
    if(fseek(file,0,SEEK_END) || (length=ftell(file))<0 || fseek(file,0,SEEK_SET)){fclose(file);return 2;}
    json=malloc((size_t)length+1);if(!json){fclose(file);return 2;}
    read=fread(json,1,(size_t)length,file);fclose(file);json[read]=0;
    ok=read==(size_t)length && dn_catalog_validate_json(json,read,&err);
    if(!ok)fprintf(stderr,"%s\n",err.message);
    free(json);return ok?0:1;
}
