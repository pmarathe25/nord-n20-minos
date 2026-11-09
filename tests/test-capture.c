/* Verify captures cannot overwrite existing files or follow symlinks. */
#define main capture_main
#include "../dashboard/capture.c"
#undef main
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
int main(void) {
    char dir[]="/tmp/n20-capture-test-XXXXXX", path[256], linkpath[256];
    assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/frame",dir);snprintf(linkpath,sizeof(linkpath),"%s/link",dir);
    mode_t old=umask(0);int fd=open_output(path);umask(old);assert(fd>=0);close(fd);
    struct stat st;assert(stat(path,&st)==0 && (st.st_mode & 0777)==0600);
    assert(open_output(path)<0);assert(symlink(path,linkpath)==0);assert(open_output(linkpath)<0);
    assert(unlink(linkpath)==0);assert(unlink(path)==0);assert(rmdir(dir)==0);
    puts("CAPTURE_PERMISSIONS_AND_SYMLINK_GUARD_PASSED");return 0;
}
