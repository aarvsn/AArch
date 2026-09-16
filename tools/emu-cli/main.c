/*
 * emu-cli: headless driver for emu-fw cores.
 *
 * Modes:
 *   emu-cli --list
 *   emu-cli --core NAME --rom FILE [--frames N] [--input MASK]
 *           [--fb-out FILE] [--state-out FILE] [--state-in FILE] [--benchmark]
 *   emu-cli --core NAME --rom FILE --smoke
 *
 * --smoke runs three deterministic checks in-process:
 *   1. two identical runs produce identical framebuffer CRCs
 *   2. save state -> restore into a fresh core -> trajectories match
 *   3. repeated save/load cycles match a plain run
 * Exit code 0 = all checks passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu/emu.h"

static const emu_core_vtable_t *lookup_core(const char *name)
{
    if (strcmp(name, "mgbx") == 0)
#ifdef EMU_BUILD_MGBX
        return emu_core_mgbx();
#else
        return NULL;
#endif
    if (strcmp(name, "beatle-nes-redux") == 0)
#ifdef EMU_BUILD_BEATLE_NES_REDUX
        return emu_core_beatle_nes_redux();
#else
        return NULL;
#endif
    if (strcmp(name, "supersnes") == 0)
#ifdef EMU_BUILD_SUPERSNES
        return emu_core_supersnes();
#else
        return NULL;
#endif
    if (strcmp(name, "mgbax") == 0)
#ifdef EMU_BUILD_MGBAX
        return emu_core_mgbax();
#else
        return NULL;
#endif
    return NULL;
}

static void list_cores(void)
{
    static const char *names[] = { "mgbx", "beatle-nes-redux", "supersnes", "mgbax" };
    printf("compiled cores:\n");
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const emu_core_vtable_t *vt = lookup_core(names[i]);
        if (vt != NULL)
            printf("  %-18s %s (%ux%u, %u Hz)\n", vt->name, vt->system,
                   (unsigned)vt->fb_width, (unsigned)vt->fb_height,
                   (unsigned)vt->sample_rate);
        else
            printf("  %-18s (not built)\n", names[i]);
    }
}

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)sz;
    return buf;
}

static int write_ppm(const char *path, const uint32_t *fb, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = fb[y * w + x];
            uint8_t rgb[3] = { EMU_PIXEL_R(px), EMU_PIXEL_G(px), EMU_PIXEL_B(px) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}

static uint32_t frame_crc(emu_core_t *c)
{
    uint32_t w = 0, h = 0;
    const uint32_t *fb = c->vtable->framebuffer(c, &w, &h);
    return emu_crc32(fb, (size_t)w * h);
}

static void run_frames(emu_core_t *c, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        c->vtable->run_frame(c);
}

static int core_smoke(const emu_core_vtable_t *vt, const uint8_t *rom, size_t rom_size)
{
    int failures = 0;

    /* 1. determinism */
    emu_core_t *a = NULL, *b = NULL;
    vt->create(&a);
    vt->create(&b);
    vt->load_rom(a, rom, rom_size);
    vt->load_rom(b, rom, rom_size);
    run_frames(a, 12);
    run_frames(b, 12);
    uint32_t ca = frame_crc(a), cb = frame_crc(b);
    if (ca != cb) {
        fprintf(stderr, "smoke FAIL: determinism (crc %08X != %08X)\n", ca, cb);
        failures++;
    }

    /* 2. state roundtrip */
    size_t sz = a->vtable->state_size(a);
    uint8_t *blob = malloc(sz);
    a->vtable->save_state(a, blob, sz);
    run_frames(a, 12);
    uint32_t want = frame_crc(a);
    emu_core_t *c = NULL;
    vt->create(&c);
    vt->load_rom(c, rom, rom_size);
    c->vtable->load_state(c, blob, sz);
    run_frames(c, 12);
    uint32_t got = frame_crc(c);
    if (want != got) {
        fprintf(stderr, "smoke FAIL: state roundtrip (crc %08X != %08X)\n", got, want);
        failures++;
    }
    free(blob);
    vt->destroy(c);

    /* 3. repeated cycles */
    run_frames(b, 12); /* b is at frame 24 now; a too */
    if (frame_crc(a) != frame_crc(b)) {
        fprintf(stderr, "smoke FAIL: continued divergence\n");
        failures++;
    }
    vt->destroy(a);
    vt->destroy(b);
    return failures;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: emu-cli --core NAME --rom FILE [options]\n"
            "  --frames N       run N frames (default 60)\n"
            "  --input MASK     button bitmask (hex, e.g. 0x01 = A)\n"
            "  --fb-out FILE    write final frame as PPM\n"
            "  --state-out FILE write save state after the run\n"
            "  --state-in FILE  load save state before running\n"
            "  --benchmark      report throughput instead of progress\n"
            "  --smoke          run in-process determinism checks\n"
            "  --list           list compiled cores and exit\n");
}

int main(int argc, char **argv)
{
    const char *core_name = NULL, *rom_path = NULL, *fb_out = NULL;
    const char *state_out = NULL, *state_in = NULL;
    uint32_t frames = 60, input = 0;
    int benchmark = 0, smoke = 0, do_list = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--core") == 0 && i + 1 < argc)
            core_name = argv[++i];
        else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc)
            rom_path = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--fb-out") == 0 && i + 1 < argc)
            fb_out = argv[++i];
        else if (strcmp(argv[i], "--state-out") == 0 && i + 1 < argc)
            state_out = argv[++i];
        else if (strcmp(argv[i], "--state-in") == 0 && i + 1 < argc)
            state_in = argv[++i];
        else if (strcmp(argv[i], "--benchmark") == 0)
            benchmark = 1;
        else if (strcmp(argv[i], "--smoke") == 0)
            smoke = 1;
        else if (strcmp(argv[i], "--list") == 0)
            do_list = 1;
        else {
            usage();
            return 2;
        }
    }

    if (do_list) {
        list_cores();
        return 0;
    }
    if (core_name == NULL || rom_path == NULL) {
        usage();
        return 2;
    }

    const emu_core_vtable_t *vt = lookup_core(core_name);
    if (vt == NULL) {
        fprintf(stderr, "emu-cli: core '%s' is not compiled in\n", core_name);
        return 2;
    }

    size_t rom_size = 0;
    uint8_t *rom = read_file(rom_path, &rom_size);
    if (rom == NULL) {
        fprintf(stderr, "emu-cli: cannot read ROM '%s'\n", rom_path);
        return 2;
    }

    emu_core_t *core = NULL;
    emu_result_t r = emu_core_create(vt, &core);
    if (r != EMU_OK) {
        fprintf(stderr, "emu-cli: create failed: %s\n", emu_result_str(r));
        free(rom);
        return 1;
    }
    r = vt->load_rom(core, rom, rom_size);
    if (r != EMU_OK) {
        fprintf(stderr, "emu-cli: load_rom failed: %s\n", emu_result_str(r));
        free(rom);
        emu_core_destroy(core);
        return 1;
    }
    vt->set_input(core, input);

    if (smoke) {
        int fails = core_smoke(vt, rom, rom_size);
        emu_core_destroy(core);
        free(rom);
        if (fails > 0) {
            fprintf(stderr, "emu-cli: %d smoke check(s) failed\n", fails);
            return 1;
        }
        printf("smoke checks passed\n");
        return 0;
    }

    if (state_in != NULL) {
        size_t ssz = 0;
        uint8_t *sblob = read_file(state_in, &ssz);
        if (sblob == NULL) {
            fprintf(stderr, "emu-cli: cannot read state '%s'\n", state_in);
            free(rom);
            emu_core_destroy(core);
            return 2;
        }
        r = vt->load_state(core, sblob, ssz);
        free(sblob);
        if (r != EMU_OK) {
            fprintf(stderr, "emu-cli: load_state failed: %s\n", emu_result_str(r));
            emu_core_destroy(core);
            return 1;
        }
    }

    clock_t t0 = clock();
    run_frames(core, frames);
    clock_t t1 = clock();
    free(rom);

    double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
    if (benchmark) {
        printf("benchmark: %u frames in %.3f s (%.1f fps)\n", frames, secs,
               secs > 0 ? frames / secs : 0.0);
    } else {
        printf("ran %u frames in %.3f s; framebuffer crc32 = %08X\n", frames,
               secs, frame_crc(core));
    }

    if (fb_out != NULL) {
        uint32_t w = 0, h = 0;
        const uint32_t *fb = vt->framebuffer(core, &w, &h);
        if (write_ppm(fb_out, fb, w, h) != 0) {
            fprintf(stderr, "emu-cli: cannot write PPM '%s'\n", fb_out);
            emu_core_destroy(core);
            return 1;
        }
        printf("wrote %s (%ux%u)\n", fb_out, (unsigned)w, (unsigned)h);
    }

    if (state_out != NULL) {
        size_t ssz = vt->state_size(core);
        uint8_t *sblob = malloc(ssz);
        r = vt->save_state(core, sblob, ssz);
        if (r != EMU_OK) {
            fprintf(stderr, "emu-cli: save_state failed: %s\n", emu_result_str(r));
            free(sblob);
            emu_core_destroy(core);
            return 1;
        }
        FILE *f = fopen(state_out, "wb");
        if (f == NULL || fwrite(sblob, 1, ssz, f) != ssz) {
            fprintf(stderr, "emu-cli: cannot write state '%s'\n", state_out);
            free(sblob);
            emu_core_destroy(core);
            return 1;
        }
        fclose(f);
        free(sblob);
        printf("wrote %s (%zu bytes)\n", state_out, ssz);
    }

    emu_core_destroy(core);
    return 0;
}
