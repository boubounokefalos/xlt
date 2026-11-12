/* XLT - .m3u / .m3u8 playlists: reading and writing the text. Portable
 * (host-tested); the files themselves are read and written by playlists.c. */
#ifndef M3U_H
#define M3U_H

#define M3U_PATH 200 /* bytes with the NUL, as Entry.path */

/* One song: its full PSP path ("ms0:/MUSIC/Artist/song.mp3") and the title
 * from the #EXTINF line before it ("" if there was none). */
typedef void (*M3uFn)(const char *path, const char *title, void *ctx);

/* Reads a playlist's text. m3u_path is where the file is (for relative
 * paths and the device). Lines in Latin-1 / Windows-1252 are turned into
 * UTF-8; lines that are already UTF-8 are kept. Comments, blank lines and
 * web addresses are skipped. Returns the number of songs given to fn. */
int m3u_parse(const char *text, int len, const char *m3u_path, M3uFn fn, void *ctx);

/* One line of a playlist as a full PSP path. 0 if it names a file. */
int m3u_resolve(const char *line, const char *m3u_path, char *out, int cap);

/* `target` written relative to the folder `dir` ("ms0:/MUSIC/PLAYLISTS" and
 * "ms0:/MUSIC/A/b.mp3" give "../A/b.mp3"); the full path if they are on
 * different devices. */
void m3u_relative(const char *dir, const char *target, char *out, int cap);

/* Writes a playlist (UTF-8, #EXTM3U, one #EXTINF per song) for the file at
 * m3u_path. titles may be NULL or hold NULL / "" entries. Returns the length,
 * or -1 if it doesn't fit in cap. */
int m3u_write(char *out, int cap, const char *m3u_path, const char *const *paths, const char *const *titles, int n);

/* 1 if the file name ends in .m3u or .m3u8. */
int m3u_is_playlist(const char *name);

#endif
