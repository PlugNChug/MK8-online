#include "mods.h"
#include "romfs_build.h"
#include "usermods.h"
#include "hle.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rt_log(const char *fmt, ...) { (void)fmt; }
void game_modules_loaded(void) {}
void rt_fatal_trace(void) { abort(); }
bool plat_exe_path(char *path, size_t cap) { return snprintf(path,cap,"./lookup.exe") < (int)cap; }
bool plat_remove_tree(const char *path) { (void)path; abort(); }
bool mk8_nextendo_enabled(void) { return !strcmp(getenv("MK8_NEXTENDO_TEST"),"1"); }
void mk8_settings_button_build(uint64_t page) { abort(); }
void mk8_settings_button_hover(uint64_t page,unsigned instant) { abort(); }
void mk8_settings_page_build(uint64_t page) { abort(); }
void mk8_settings_page_tick(uint64_t page,bool open) { abort(); }
bool mk8_settings_layout_available(void);  // (romfs_overlay.c: the native Menu layout is layered)
bool mk8_controller_layout_available(void);

__attribute__((section(".rst$a"),used)) static const ResetFn resets_start[1];
__attribute__((section(".rst$z"),used)) static const ResetFn resets_end[1];
static void soft_reset(void) {
    const ResetFn *start=resets_start, *end=resets_end;
    __asm__("" : "+r"(start), "+r"(end));
    for (const ResetFn *p=start; p<end; ++p) if (*p) (*p)();
}

static void check_file(const char *path, const char *expected) {
    char data[128]={0};
    FILE *f=fopen(path,"rb");
    assert(f);
    size_t n=fread(data,1,sizeof data,f);
    fclose(f);
    assert(n==strlen(expected) && !memcmp(data,expected,n));
}

static void read_asset(const char *asset,const char *expected) {
    char why[256], host[1024];
    RomfsIndex ix={0};
    assert(romfs_index("base",mods_overlay(),&ix,why,sizeof why));
    for(int i=0;i<ix.nfiles;i++) {
        if(strcmp(ix.files[i].path,asset)) continue;
        check_file(ix.files[i].host,expected);
        if(mods_romfs_override(asset,host,sizeof host)) check_file(host,expected);
        else assert(!strcmp(expected,"base"));
        return;
    }
    assert(!"asset missing from RomFS index");
}

int main(int argc, char **argv) {
    if (argc==3 && !strcmp(argv[1],"full-list")) {
        assert(usermods_count()==128);
        const char *last=argv[2];
        read_asset("Course/sample.bin",last);
        assert(!usermods_changed_since_start());
        usermods_set_enabled(127,false,NULL,0);
        assert(usermods_changed_since_start());
        return 0;
    }
    if (argc==2 && !strcmp(argv[1],"soft-reset")) {
        read_asset("Course/sample.bin","base");
        usermods_set_enabled(0,true,NULL,0);
        assert(usermods_changed_since_start());
        read_asset("Course/sample.bin","base");
        soft_reset();
        read_asset("Course/sample.bin","first");
        assert(!usermods_changed_since_start());
        usermods_set_enabled(0,false,NULL,0);
        soft_reset();
        read_asset("Course/sample.bin","base");
        assert(!usermods_changed_since_start());
        return 0;
    }
    if (argc==2) {
        assert(usermods_count()==2);
        UserMod mod;
        assert(usermods_get(0,&mod) && !strcmp(mod.id,"A"));
        bool enable=!strcmp(argv[1],"enable");
        assert(mod.enabled!=enable);
        usermods_set_enabled(0,enable,NULL,0);
        return 0;
    }
    assert(argc==4 || argc==6);
    read_asset(argv[2],argv[3]);
    if (argc==6) {
        assert(mk8_settings_layout_available()==atoi(argv[4]));
        assert(mk8_controller_layout_available()==atoi(argv[5]));
    }
    return 0;
}
