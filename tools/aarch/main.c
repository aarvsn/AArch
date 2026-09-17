/*
 * aarch: headless CLI driver for AArch cores.
 *
 * Modes:
 *   aarch --list
 *   aarch --rom FILE [--core NAME] [--frames N] [--input MASK]
 *         [--fb-out FILE] [--state-out FILE] [--state-in FILE] [--benchmark]
 *   aarch --core NAME --rom FILE --smoke
 *
 * Without --core the core is auto-selected by ROM signature probing
 * (emu_rom_probe). --core always overrides the probe. Skeleton cores
 * report "not implemented" and exit with status 3.
 *
 * Exit codes: 0 ok, 1 runtime failure, 2 usage/IO error, 3 not implemented,
 *             4 smoke check failure.
 *
 * The CLI uses only C11 stdio so the same binary builds on Windows 7+
 * (MinGW or MSVC), macOS 10.12+, Linux, and Android 6+ (NDK, API 23).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu/emu.h"

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

static void list_cores(void)
{
    size_t count = 0;
    const emu_core_info_t *reg = emu_core_registry(&count);
    printf("AArch cores:\n");
    for (size_t i = 0; i < count; i++) {
        const emu_core_vtable_t *vt = reg[i].vtable ? reg[i].vtable() : NULL;
        if (vt != NULL)
            printf("  %-16s %-8s %-30s (%ux%u, %u Hz)\n", reg[i].name,
                   emu_core_status_str(reg[i].status), reg[i].note,
                   (unsigned)vt->fb_width, (unsigned)vt->fb_height,
                   (unsigned)vt->sample_rate);
        else
            printf("  %-16s %-8s %-30s (not built)\n", reg[i].name,
                   emu_core_status_str(reg[i].status), reg[i].note);
    }
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
    if (sz > 0) {
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
            fprintf(stderr, "smoke FAIL: state roundtrip (crc %08X != %08X)\n",
                    got, want);
            failures++;
        }
        free(blob);
        vt->destroy(c);
    }

    /* 3. continued run parity */
    run_frames(b, 12);
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
            "usage: aarch --rom FILE [--core NAME] [options]\n"
            "  --core NAME      force a specific core (default: auto-detect)\n"
            "  --frames N       run N frames (default 60)\n"
            "  --input MASK     button bitmask (hex)\n"
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
    if (rom_path == NULL) {
        usage();
        return 2;
    }

    size_t count = 0;
    const emu_core_info_t *reg = emu_core_registry(&count);
    const emu_core_info_t *info = NULL;

    if (core_name != NULL) {
        for (size_t i = 0; i < count; i++) {
            if (strcmp(reg[i].name, core_name) == 0) {
                info = &reg[i];
                break;
            }
        }
        if (info == NULL) {
            fprintf(stderr, "aarch: unknown core '%s'\n", core_name);
            return 2;
        }
    } else {
        size_t rom_size = 0;
        uint8_t *rom = read_file(rom_path, &rom_size);
        if (rom == NULL) {
            fprintf(stderr, "aarch: cannot read ROM '%s'\n", rom_path);
            return 2;
        }
        const char *guess = emu_rom_probe(rom, rom_size);
        free(rom);
        for (size_t i = 0; i < count; i++) {
            if (strcmp(reg[i].name, guess) == 0) {
                info = &reg[i];
                break;
            }
        }
        if (info == NULL || info->vtable == NULL) {
            fprintf(stderr, "aarch: cannot identify ROM as a built core "
                            "(probe: %s); use --core NAME\n",
                    guess ? guess : "unknown");
            return 2;
        }
    }

    if (info->vtable == NULL) {
        fprintf(stderr, "aarch: core '%s' is not compiled in\n", info->name);
        return 2;
    }
    const emu_core_vtable_t *vt = info->vtable();

    size_t rom_size = 0;
    uint8_t *rom = read_file(rom_path, &rom_size);
    if (rom == NULL) {
        fprintf(stderr, "aarch: cannot read ROM '%s'\n", rom_path);
        return 2;
    }

    emu_core_t *core = NULL;
    emu_result_t r = emu_core_create(vt, &core);
    if (r != EMU_OK) {
        fprintf(stderr, "aarch: create failed: %s\n", emu_result_str(r));
        free(rom);
        return 1;
    }
    r = vt->load_rom(core, rom, rom_size);
    if (r != EMU_OK) {
        fprintf(stderr, "aarch: load_rom failed: %s\n", emu_result_str(r));
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
            fprintf(stderr, "aarch: %d smoke check(s) failed\n", fails);
            return 4;
        }
        printf("smoke checks passed\n");
        return 0;
    }

    if (state_in != NULL) {
        size_t ssz = 0;
        uint8_t *sblob = read_file(state_in, &ssz);
        if (sblob == NULL) {
            fprintf(stderr, "aarch: cannot read state '%s'\n", state_in);
            free(rom);
            emu_core_destroy(core);
            return 2;
        }
        r = vt->load_state(core, sblob, ssz);
        free(sblob);
        if (r != EMU_OK) {
            fprintf(stderr, "aarch: load_state failed: %s\n", emu_result_str(r));
            emu_core_destroy(core);
            return 1;
        }
    }

    clock_t t0 = clock();
    r = EMU_OK;
    for (uint32_t i = 0; i < frames; i++) {
        emu_result_t fr = vt->run_frame(core);
        if (fr != EMU_OK) {
            r = fr;
            break;
        }
    }
    clock_t t1 = clock();
    free(rom);

    if (r == EMU_ENOTIMPL) {
        fprintf(stderr, "aarch: core '%s' is a skeleton: emulation is not "
                        "implemented for this system\n", info->name);
        emu_core_destroy(core);
        return 3;
    }
    if (r != EMU_OK) {
        fprintf(stderr, "aarch: run_frame failed: %s\n", emu_result_str(r));
        emu_core_destroy(core);
        return 1;
    }

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
            fprintf(stderr, "aarch: cannot write PPM '%s'\n", fb_out);
            emu_core_destroy(core);
            return 1;
        }
        printf("wrote %s (%ux%u)\n", fb_out, (unsigned)w, (unsigned)h);
    }

    if (state_out != NULL) {
        size_t ssz = vt->state_size(core);
        if (ssz == 0) {
            fprintf(stderr, "aarch: core '%s' does not support save states\n",
                    info->name);
            emu_core_destroy(core);
            return 3;
        }
        uint8_t *sblob = malloc(ssz);
        r = vt->save_state(core, sblob, ssz);
        if (r != EMU_OK) {
            fprintf(stderr, "aarch: save_state failed: %s\n", emu_result_str(r));
            free(sblob);
            emu_core_destroy(core);
            return 1;
        }
        FILE *f = fopen(state_out, "wb");
        if (f == NULL || fwrite(sblob, 1, ssz, f) != ssz) {
            fprintf(stderr, "aarch: cannot write state '%s'\n", state_out);
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
