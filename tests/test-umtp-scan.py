#!/usr/bin/env python3
"""Test the patched upstream directory scanner using disposable files.

Usage: python3 tests/test-umtp-scan.py /path/to/patched/umtp-source
"""
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]).resolve()
harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>
#include "fs_handles_db.h"
DIR *fs_find_first_file(char *, filefoundinfo *);
int fs_find_next_file(DIR *, char *, filefoundinfo *);
int fs_find_close(DIR *);
int main(int argc, char **argv) {
    filefoundinfo info;
    if (argc != 2) return 2;
    DIR *dir = fs_find_first_file(argv[1], &info);
    if (!dir) return 3;
    do { puts(info.filename); } while (fs_find_next_file(dir, argv[1], &info));
    return fs_find_close(dir);
}
'''
with tempfile.TemporaryDirectory(prefix='n20-umtp-scan-') as tmp:
    root = pathlib.Path(tmp)
    test_c = root / 'scan.c'
    test_c.write_text(harness)
    binary = root / 'scan'
    subprocess.run(['cc', '-I' + str(source / 'inc'), '-ffunction-sections',
                    '-fdata-sections', str(test_c), str(source / 'src/fs_handles_db.c'),
                    '-Wl,--gc-sections', '-o', str(binary)], check=True)
    world = root / 'world'
    world.mkdir()
    (world / 'before').write_text('one')
    (world / 'broken').symlink_to('missing')
    (world / 'external').symlink_to('/etc/passwd')
    (world / 'internal').symlink_to('before')
    (world / 'after').write_text('two')
    (world / 'folder').mkdir()
    result = subprocess.check_output([binary, world], text=True).splitlines()
    assert set(result) == {'.', '..', 'before', 'after', 'folder'}, result
    print('UMTP_SCAN_SKIPS_LINKS_AND_CONTINUES_PASSED')
