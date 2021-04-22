#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"
// bofore implement this file, 
// you would better read section 8.11 of xv6 book
// or look up ls.c

char *fmtname(char *path) {
    char *p;

    for(p = path + strlen(path); p >= path && *p != '/'; --p);
    return p+1;
}

void find(char *path, char *filename) {
    char buf[512], *p;
    int fd;
    struct stat st;
    struct dirent de;

    if( (fd = open(path, 0)) < 0 ) {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }

    if(fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    switch (st.type) {
    case T_FILE:
        if(strcmp(fmtname(path), filename) == 0) printf("%s\n", path);
        break;
    case T_DIR:
        strcpy(buf, path);
        p = buf + strlen(buf);
        *p++ = '/';
        while(read(fd, &de, sizeof(de)) == sizeof(de)) {
            if(
                de.inum == 0 || 
                strcmp(de.name, ".") == 0 || 
                strcmp(de.name, "..") == 0
            ) continue;
            memmove(p, de.name, DIRSIZ);
            p[DIRSIZ] = 0;
            find(buf, filename);
        }
        break;
    default:
        break;
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if(argc < 3) {
        fprintf(2, "usage: find files in dir...\n");
        exit(0);
    }
    find(argv[1], argv[2]);
    exit(0);
}
