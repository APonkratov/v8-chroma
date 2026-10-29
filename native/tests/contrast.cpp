#include "../src/protocol.h"
#include "../src/contrast.h"
#include <stdio.h>
#include <wchar.h>

int wmain(int argc,wchar_t** argv) {
    if (argc<2) return 2;
    unsigned tested=0,corrected=0;
    for (int i=1;i<argc;++i) {
        ProbeConfig cfg={}; FILE* file=_wfopen(argv[i],L"rb");
        if (!file) return 3;
        size_t read=fread(&cfg,1,sizeof(cfg),file); fclose(file);
        if (read!=sizeof(cfg) || cfg.version!=3 || cfg.mapCount>256) return 4;
        if (!cfg.mode) continue;
        DWORD background=0; bool foundBackground=false;
        for (DWORD j=0;j<cfg.mapCount;++j)
            if (cfg.maps[j].kind==CairoRgb && cfg.maps[j].source==0xF5F5F5) {
                background=cfg.maps[j].target; foundBackground=true;
            }
        if (!foundBackground) { printf("Missing changed-block mapping: %s\n",cfg.themeId); return 8; }
        for (DWORD j=0;j<cfg.mapCount;++j) {
            const auto& entry=cfg.maps[j];
            if (entry.kind!=CairoRgba) continue;
            bool syntax=false;
            for (DWORD source:{0u,0xFF0000u,0x008000u,0x963200u,0x0000FFu,0x0000A0u}) syntax |= source==entry.source;
            if (!syntax) continue;
            DWORD ink=ChromaContrast::readableInk(entry.target,background,cfg.colors.glyphForeground);
            bool alreadyReadable=ChromaContrast::ratio(entry.target,background)>=4.5;
            if (ChromaContrast::ratio(ink,background)<4.5 || (alreadyReadable && ink!=entry.target)) {
                printf("Unreadable syntax: %s source %06lX\n",cfg.themeId,entry.source); return 5;
            }
            ++tested; corrected += ink!=entry.target;
        }
    }
    // The exact pair reported by CodeRabbit, rather than a text-color-only test.
    DWORD comment=ChromaContrast::readableInk(0x6272A4,0x1E4E5A,0xF8F8F2);
    if (comment==0x6272A4 || ChromaContrast::ratio(comment,0x1E4E5A)<4.5) return 6;
    printf("Changed-block syntax contrast: %u pairs, %u adjusted, Dracula comment %06lX: PASS\n",tested,corrected,comment);
    return tested>=13*6?0:7;
}
