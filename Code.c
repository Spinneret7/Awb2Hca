/*
 * Awb2hca - extracts the audio files from an AWB archive
 *
 * Build:  gcc awb2hca.c -o Awb2hca
 * Usage:  Awb2hca <path to .AWB file>
 */

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <direct.h>
  #define strcasecmp _stricmp
  #define MKDIR(p) _mkdir(p)
  #define PATH_SEP '\\'
  #define FSEEK _fseeki64
  typedef __int64 fpos_t64;
#else
  #include <strings.h>
  #include <unistd.h>
  #define MKDIR(p) mkdir((p), 0777)
  #define PATH_SEP '/'
  #define FSEEK fseeko
  typedef off_t fpos_t64;
#endif

#define PATH_MAX_LEN 4096

/* ---------- error handling (replaces C# exceptions) ---------- */

static int g_err = 0;
static const char *g_errmsg = "";

static void fail(const char *msg)
{
    if (!g_err) {
        g_err = 1;
        g_errmsg = msg;
    }
}

/* ---------- little-endian binary helpers ---------- */

static uint8_t rd_u8(FILE *f)
{
    int c = fgetc(f);
    if (c == EOF) { fail("Unable to read beyond the end of the stream."); return 0; }
    return (uint8_t)c;
}

static uint16_t rd_u16(FILE *f)
{
    uint16_t a = rd_u8(f);
    uint16_t b = rd_u8(f);
    return (uint16_t)(a | (b << 8));
}

static uint32_t rd_u32(FILE *f)
{
    uint32_t a = rd_u16(f);
    uint32_t b = rd_u16(f);
    return a | (b << 16);
}

static void wr_u8(FILE *f, uint8_t v)
{
    if (fputc(v, f) == EOF) fail("Unable to write to file.");
}

static void wr_u16(FILE *f, uint16_t v)
{
    wr_u8(f, (uint8_t)(v & 0xFF));
    wr_u8(f, (uint8_t)(v >> 8));
}

static void wr_bytes(FILE *f, const void *data, size_t len)
{
    if (len > 0 && fwrite(data, 1, len, f) != len) fail("Unable to write to file.");
}

/* ---------- misc helpers ---------- */

static uint32_t GetRealAddress(uint32_t ptr, uint32_t targetPadding)
{
    return ptr + (targetPadding - (ptr % targetPadding)) % targetPadding;
}

static const char *GetExtensionFromMagic(int32_t Magic)
{
    if (Magic == 0x00414348) {
        return ".hca";
    } else if (Magic == 0x46464952) {
        return ".at9";
    } else return ".adx";
}

static int EndsWithNoCase(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (ls < lx) return 0;
    return strcasecmp(s + ls - lx, suffix) == 0;
}

static int IsSep(char c)
{
    return c == '/' || c == '\\';
}

/* Path.Join equivalent */
static void PathJoin(char *out, size_t size, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la == 0) {
        snprintf(out, size, "%s", b);
    } else if (IsSep(a[la - 1])) {
        snprintf(out, size, "%s%s", a, b);
    } else {
        snprintf(out, size, "%s%c%s", a, PATH_SEP, b);
    }
}

/* Path.GetDirectoryName equivalent (result may be empty) */
static void GetDirectoryName(char *out, size_t size, const char *path)
{
    size_t len = strlen(path), i;
    for (i = len; i > 0; i--) {
        if (IsSep(path[i - 1])) break;
    }
    if (i == 0) { out[0] = '\0'; return; }
    size_t dirlen = i - 1;
    if (dirlen == 0) dirlen = 1; /* root "/" */
    if (dirlen >= size) dirlen = size - 1;
    memcpy(out, path, dirlen);
    out[dirlen] = '\0';
}

/* Path.GetFileNameWithoutExtension equivalent */
static void GetFileNameWithoutExtension(char *out, size_t size, const char *path)
{
    size_t len = strlen(path), i;
    char *dot;
    for (i = len; i > 0; i--) {
        if (IsSep(path[i - 1])) break;
    }
    snprintf(out, size, "%s", path + i);
    dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* ---------- extraction ---------- */

static void ExtractAWB(const char *filePath)
{
    FILE *reader = NULL;
    uint32_t *PtrList = NULL;
    int32_t *WaveID = NULL;
    int ptrCount = 0;
    int i;

    reader = fopen(filePath, "rb");
    if (!reader) { fail("Could not open the input file."); return; }

    FSEEK(reader, 6, SEEK_SET);

    uint16_t WaveFieldLength = rd_u16(reader);

    int32_t numOfEntries = (int32_t)rd_u32(reader);
    uint16_t targetPadding = rd_u16(reader); /* usually 0x20 */
    uint16_t awb_key = rd_u16(reader);

    if (g_err) goto cleanup;
    if (numOfEntries < 0) { fail("Arithmetic operation resulted in an overflow."); goto cleanup; }
    if (targetPadding == 0) { fail("Attempted to divide by zero."); goto cleanup; }

    PtrList = (uint32_t *)calloc((size_t)numOfEntries + 1, sizeof(uint32_t));
    WaveID = (int32_t *)calloc((size_t)numOfEntries + 1, sizeof(int32_t));
    if (!PtrList || !WaveID) { fail("Out of memory."); goto cleanup; }

    for (i = 0; i < numOfEntries; i++) {
        if (WaveFieldLength == 2) WaveID[i] = rd_u16(reader);
        else WaveID[i] = (int32_t)rd_u32(reader);
        if (g_err) goto cleanup;
    }

    for (i = 0; i < numOfEntries; i++) {
        PtrList[ptrCount++] = GetRealAddress(rd_u32(reader), targetPadding);
        if (g_err) goto cleanup;
    }

    PtrList[ptrCount++] = rd_u32(reader); /* filesize */
    if (g_err) goto cleanup;

    {
        char dirName[PATH_MAX_LEN];
        char baseName[PATH_MAX_LEN];
        char BaseDirectory[PATH_MAX_LEN];

        GetDirectoryName(dirName, sizeof(dirName), filePath);
        GetFileNameWithoutExtension(baseName, sizeof(baseName), filePath);
        PathJoin(BaseDirectory, sizeof(BaseDirectory), dirName, baseName);

        MKDIR(BaseDirectory); /* ignores "already exists" */

        if (awb_key > 0) {
            char keypath[PATH_MAX_LEN];
            PathJoin(keypath, sizeof(keypath), BaseDirectory, ".subkey_streaming");

            FILE *writer = fopen(keypath, "wb");
            if (!writer) { fail("Could not create key file."); goto cleanup; }
            printf("AWBExtractRaw: Writing key to %s\n", keypath);
            wr_u16(writer, awb_key);
            fclose(writer);
            if (g_err) goto cleanup;
        }

        for (i = 0; i < ptrCount - 1; i++) {
            int32_t fileSize = (int32_t)(PtrList[i + 1] - PtrList[i]);
            FSEEK(reader, (fpos_t64)PtrList[i], SEEK_SET);
            int32_t Magic = (int32_t)rd_u32(reader);
            if (g_err) goto cleanup;
            FSEEK(reader, (fpos_t64)PtrList[i], SEEK_SET);

            if (fileSize < 0) { fail("Non-negative number required."); goto cleanup; }

            unsigned char *remainingData = (unsigned char *)malloc(fileSize > 0 ? (size_t)fileSize : 1);
            if (!remainingData) { fail("Out of memory."); goto cleanup; }
            size_t got = fread(remainingData, 1, (size_t)fileSize, reader);

            char fname[256];
            char targetPath[PATH_MAX_LEN];
            snprintf(fname, sizeof(fname), "%05d_streaming%s", (int)WaveID[i], GetExtensionFromMagic(Magic));
            PathJoin(targetPath, sizeof(targetPath), BaseDirectory, fname);

            FILE *writer = fopen(targetPath, "wb");
            if (!writer) { free(remainingData); fail("Could not create output file."); goto cleanup; }
            printf("AWBExtractRaw: Writing file %s\n", targetPath);
            wr_bytes(writer, remainingData, got);
            fclose(writer);
            free(remainingData);
            if (g_err) goto cleanup;
        }
    }

cleanup:
    if (reader) fclose(reader);
    free(PtrList);
    free(WaveID);
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    if (argc < 2 || strcasecmp(argv[1], "-h") == 0 || strcasecmp(argv[1], "-help") == 0) {
        printf("Usage: Awb2hca <path to .AWB file>\n");
        return 0;
    }

    const char *filePath = argv[1];
    if (!EndsWithNoCase(filePath, ".awb")) {
        printf("Awb2hca: Input File is not an AWB file!\n");
        return 0;
    }

    ExtractAWB(filePath);

    if (g_err) {
        printf("An error occurred: %s\n", g_errmsg);
        return 1;
    }

    return 0;
}