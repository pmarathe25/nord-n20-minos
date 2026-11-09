#!/usr/bin/env python3
"""Compare a local server JAR with the complete official Mojang download."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import urllib.parse
import urllib.request

MANIFEST='https://piston-meta.mojang.com/mc/game/version_manifest_v2.json'
MAX_METADATA=5*1024*1024
MAX_JAR=512*1024*1024


def require_host(url,host):
    parsed=urllib.parse.urlsplit(url)
    if parsed.scheme!='https' or parsed.hostname!=host or parsed.username is not None or parsed.password is not None or parsed.port not in (None,443):
        raise ValueError('unexpected publisher URL')


class PublisherRedirect(urllib.request.HTTPRedirectHandler):
    def __init__(self,host):super().__init__();self.host=host
    def redirect_request(self,req,fp,code,msg,headers,newurl):
        require_host(newurl,self.host)
        return super().redirect_request(req,fp,code,msg,headers,newurl)


def open_publisher(url,host):
    require_host(url,host)
    return urllib.request.build_opener(PublisherRedirect(host)).open(url,timeout=30)


def metadata(url):
    with open_publisher(url,'piston-meta.mojang.com') as response:
        data=response.read(MAX_METADATA+1)
    if len(data)>MAX_METADATA:raise ValueError('publisher metadata too large')
    return data


def hash_stream(source,limit):
    sha1=hashlib.sha1();sha256=hashlib.sha256();size=0
    for block in iter(lambda:source.read(1024*1024),b''):
        size+=len(block)
        if size>limit:raise ValueError('server JAR too large')
        sha1.update(block);sha256.update(block)
    return size,sha1.hexdigest(),sha256.hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jar',required=True,type=Path);parser.add_argument('--version',required=True)
    args=parser.parse_args()
    versions=json.loads(metadata(MANIFEST))['versions']
    entries=[v for v in versions if v.get('id')==args.version]
    if len(entries)!=1:raise ValueError('version missing or ambiguous')
    entry=entries[0];raw=metadata(entry['url'])
    if hashlib.sha1(raw).hexdigest()!=entry['sha1']:raise ValueError('version metadata hash mismatch')
    info=json.loads(raw)
    if info['id']!=args.version:raise ValueError('version metadata ID mismatch')
    server=info['downloads']['server'];expected=server['sha1'];size=server['size']
    if not isinstance(size,int) or isinstance(size,bool) or not 0<size<=MAX_JAR or not isinstance(expected,str) or not re.fullmatch(r'[0-9a-f]{40}',expected):raise ValueError('invalid publisher server metadata')
    with open_publisher(server['url'],'piston-data.mojang.com') as response:
        published=hash_stream(response,size)
    if published[0]!=size or published[1]!=expected:raise ValueError('publisher download does not match metadata')
    fd=os.open(args.jar,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
    try:
        st=os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_nlink!=1 or st.st_size>MAX_JAR:raise ValueError('unsafe local server JAR')
        stream=os.fdopen(fd,'rb');fd=-1
        with stream:local=hash_stream(stream,MAX_JAR)
    finally:
        if fd>=0:os.close(fd)
    if local!=published:raise ValueError('local JAR differs from publisher download')
    print('Verified version:',args.version)
    print('SHA-256:',local[2])


if __name__=='__main__':main()
