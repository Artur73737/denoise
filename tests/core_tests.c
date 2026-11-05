#include "denoise.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void) {
    int x;
    const DnModel *m;
    for (x = -1000; x < 1000; ++x) {
        REQUIRE(dn_reflect(x, 1) == 0);
        REQUIRE(dn_reflect(x, 3) >= 0 && dn_reflect(x, 3) < 3);
    }
    REQUIRE(dn_reflect(-1, 3) == 1);
    REQUIRE(dn_reflect(3, 3) == 1);
    REQUIRE(dn_model_select(0, 33, "auto")->channels == 3);
    REQUIRE(!strcmp(dn_model_select(1, 34, "auto")->id, "scunet_gray_25"));
    REQUIRE(!strcmp(dn_model_select(0, 67, "auto")->id, "scunet_color_50"));
    REQUIRE(dn_model_select(1, 100, "real")->channels == 3);
    REQUIRE(dn_model_select(0, 50, "invalid") == NULL);
    m = dn_model_select(0, 1, "50");
    REQUIRE(!strcmp(m->id, "scunet_color_50"));
    REQUIRE(dn_model_count() == 7);
    REQUIRE(!strcmp(dn_model_select(0, 100, "auto")->id, "scunet_color_50"));
    REQUIRE(!strcmp(dn_model_select(1, 100, "auto")->id, "scunet_gray_50"));
    REQUIRE(!strcmp(dn_model_select(0, 100, "scunet_color_15")->id, "scunet_color_15"));
    REQUIRE(dn_model_find("SCUNET_COLOR_50.onnx") == dn_model_at(2));
    REQUIRE(dn_model_find("../scunet_color_50.onnx") == NULL);
    for (x = 0; x < (int)dn_model_count(); ++x) {
        m = dn_model_at(x);
        REQUIRE(strlen(m->graph_hash) == 64 && strlen(m->weights_hash) == 64);
        REQUIRE(m->description && m->performance && m->title);
    }
    puts("Core tests OK");
    return 0;
}
