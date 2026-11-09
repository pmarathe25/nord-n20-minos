#!/usr/bin/env python3
"""Prepare a root-controlled Minecraft bundle; never overwrite an installation."""
import argparse
import hashlib
import hmac
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
import zipfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--jar',required=True,type=Path)
    p.add_argument('--sha256',required=True,help='expected SHA-256 from a trusted source')
    p.add_argument('--output',required=True,type=Path,help='new directory in a root-controlled parent')
    a=p.parse_args()
    if os.geteuid()!=0:p.error('must run as root')
    os.umask(0o022)
    if not re.fullmatch(r'[0-9a-f]{64}',a.sha256):p.error('SHA-256 must be 64 lowercase hex digits')
    if not a.output.is_absolute() or a.output.name in ('','.','..'):p.error('output must be an absolute new directory')
    parent=a.output.parent.resolve(strict=True)
    info=parent.stat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid!=0 or info.st_mode&0o022:p.error('output parent must be root-controlled')
    target=parent/a.output.name
    if os.path.lexists(target):p.error('output already exists')
    fd=os.open(a.jar,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
    stage=None
    try:
        info=os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink!=1 or info.st_size>512*1024*1024:raise ValueError('unsafe server JAR')
        stage=Path(tempfile.mkdtemp(prefix='.n20-server-',dir=parent));archive=stage/'server.jar'
        digest=hashlib.sha256();src=os.fdopen(fd,'rb');fd=-1;copied=0
        with src,archive.open('xb') as dst:
            for block in iter(lambda:src.read(1024*1024),b''):
                copied+=len(block)
                if copied>512*1024*1024:raise ValueError('server JAR is too large')
                digest.update(block);dst.write(block)
        if not hmac.compare_digest(digest.hexdigest(),a.sha256):raise ValueError('server JAR SHA-256 mismatch')
        archive.chmod(0o644);repo=stage/'repo';repo.mkdir(mode=0o755);seen=set();total=0
        with zipfile.ZipFile(archive) as z:
            for kind in ('libraries','versions'):
                lines=z.read('META-INF/'+kind+'.list').decode('utf-8').splitlines()
                if not lines or len(lines)>500:raise ValueError('invalid bundle manifest')
                for line in lines:
                    digest,_,relative=line.split('\t');parts=PurePosixPath(relative)
                    if not re.fullmatch(r'[0-9a-f]{64}',digest) or parts.is_absolute() or not parts.parts or any(x in ('.','..') for x in parts.parts):raise ValueError('invalid bundle entry')
                    member='META-INF/'+kind+'/'+relative
                    if member in seen:raise ValueError('duplicate bundle entry')
                    seen.add(member);entry=z.getinfo(member);total+=entry.file_size
                    if entry.file_size>256*1024*1024 or total>1024*1024*1024:raise ValueError('bundle is too large')
                    out=repo/kind/relative;out.parent.mkdir(mode=0o755,parents=True,exist_ok=True);check=hashlib.sha256()
                    with z.open(entry) as source,out.open('xb') as dest:
                        for block in iter(lambda:source.read(1024*1024),b''):check.update(block);dest.write(block)
                    if not hmac.compare_digest(check.hexdigest(),digest):raise ValueError('bundled file hash mismatch')
                    out.chmod(0o644)
        stage.chmod(0o755)
        if os.path.lexists(target):raise FileExistsError('output appeared during preparation')
        os.rename(stage,target);stage=None
        print('Protected server bundle prepared:',target)
    finally:
        if fd>=0:os.close(fd)
        if stage is not None:shutil.rmtree(stage)


if __name__=='__main__':main()
