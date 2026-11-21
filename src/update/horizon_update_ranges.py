"""Verify selected Windows update members against an authenticated catalogue.

The ZIP index is read from the data source and is never a source of trusted
names, sizes or hashes. Callers must provide an authenticated, validated full
catalogue and a verified inventory of reusable files for the chosen base.
"""
from contextlib import ExitStack, contextmanager
from hashlib import sha256
from http.client import HTTPException
from time import monotonic
import zlib
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import zipfile

from stage_component_bundle import (
    MAX_MANIFEST, StageError, _json, _manifest, _zip_index, _zip_preflight,
    require,
)


@contextmanager
def changed_windows_members(source, authenticated_manifest, context, selected, verified_base):
    """Yield (archive, changed) with a bounded ZIP and exact expected entries.

    verified_base maps payload names to (size, SHA-256) from the code inventory
    checked under the installation writer lock. The final installer can use
    those immutable base files once again, while staging each changed item.
    """
    require(context["platform"]=="windows-x86_64", "Selected-member download requires Windows")
    _zip_preflight(source)
    with zipfile.ZipFile(source) as archive:
        index=_zip_index(archive)
        require(index["component-bundle.json"].file_size<=MAX_MANIFEST, "Catalogue exceeds its bound")
        raw=archive.read("component-bundle.json")
        require(_json(raw)==authenticated_manifest, "ZIP does not match the signed update catalogue")
        _,components,files,closure=_manifest(raw,context,selected)
        require(set(index)==set(files)|{"component-bundle.json"}, "Public ZIP file set differs from the catalogue")
        for path,spec in files.items():
            require(index[path].file_size==spec["bytes"], "Public ZIP file length differs from the catalogue")
        selected_paths={item["path"] for identity in closure for item in components[identity]["files"]}
        changed={path:files[path] for path in sorted(selected_paths)
                 if verified_base.get(path)!=(files[path]["bytes"],files[path]["sha256"])}
        yield archive,changed


class RangeUnavailable(Exception):
    """An optional GitHub range download was not safe or available."""


class GitHubRangeSource:
    WINDOW_BYTES=2*1024*1024
    MAX_RANGE=8*1024*1024
    MAX_REQUESTS=64
    ATTEMPT_SECONDS=25
    MEMBER_BUDGET=128*1024*1024

    def __init__(self,url,length):
        self.url=url
        self.length=length
        self.position=0
        self.requests=0
        self.downloaded=0
        self.window_start=0
        self.window=b""
        self.deadline=monotonic()+self.ATTEMPT_SECONDS

    def tell(self):
        return self.position

    def seekable(self):
        return True

    def seek(self,offset,whence=0):
        start={0:0,1:self.position,2:self.length}[whence]
        self.position=start+offset
        if self.position<0:
            raise RangeUnavailable("Negative ZIP member offset")
        return self.position

    def read(self,size=-1):
        start=self.position
        end=self.length if size==-1 else min(self.length,start+size)
        if start>=end:
            return b""
        if self.window_start<=start and end<=self.window_start+len(self.window):
            self.position=end
            return self.window[start-self.window_start:end-self.window_start]
        last=min(self.length,max(end,start+self.WINDOW_BYTES))
        if (last-start>self.MAX_RANGE or self.requests>=self.MAX_REQUESTS or
            self.downloaded+last-start>=self.length):
            raise RangeUnavailable("Selected download exceeds its bounded request budget")
        headers={"User-Agent":"PGN-Horizon-Updater",
                 "Accept-Encoding":"identity","Range":f"bytes={start}-{last-1}"}
        remaining=self.deadline-monotonic()
        if remaining<=0:
            raise RangeUnavailable("The selected download exceeded its time budget")
        try:
            with urlopen(Request(self.url,headers=headers),timeout=min(6,remaining)) as reply:
                final=urlsplit(reply.geturl())
                if final.scheme!="https" or final.hostname not in ("github.com","release-assets.githubusercontent.com","objects.githubusercontent.com"):
                    raise RangeUnavailable("Unexpected GitHub artifact host")
                expected=f"bytes {start}-{last-1}/{self.length}"
                if reply.status!=206 or reply.headers.get("Content-Range")!=expected:
                    raise RangeUnavailable("GitHub did not return the requested range")
                if reply.headers.get("Content-Encoding") not in (None,"identity"):
                    raise RangeUnavailable("GitHub returned a differently encoded range")
                data=bytearray()
                while len(data)<last-start:
                    if monotonic()>=self.deadline:
                        raise RangeUnavailable("GitHub exceeded the selected download deadline")
                    chunk=reply.read1(min(128*1024,last-start-len(data)))
                    if not chunk:
                        raise RangeUnavailable("GitHub returned an incomplete range")
                    data.extend(chunk)
        except (OSError,ValueError,HTTPException) as error:
            raise RangeUnavailable("The optional GitHub range request failed") from error
        self.requests+=1
        self.downloaded+=len(data)
        self.window_start=start
        self.window=bytes(data)
        self.position=end
        return self.window[:end-start]


@contextmanager
def authenticated_ui_changes(source,manifest,context,verified_base):
    with ExitStack() as stack:
        try:
            opened=stack.enter_context(changed_windows_members(source,manifest,context,["ui"],verified_base))
        except (StageError,zipfile.BadZipFile,EOFError,zlib.error,UnicodeDecodeError) as error:
            raise RangeUnavailable("The range ZIP did not match the verified catalogue") from error
        yield opened


def copy_changed_member(archive,name,spec,output):
    digest=sha256()
    count=0
    try:
        with archive.open(name) as source:
            while chunk:=source.read(512*1024):
                count+=len(chunk)
                if count>spec["bytes"]:
                    raise RangeUnavailable("Member bytes exceed the signed catalogue")
                digest.update(chunk)
                output.write(chunk)
    except (zipfile.BadZipFile,EOFError,zlib.error,UnicodeDecodeError) as error:
        raise RangeUnavailable("A selected ZIP member could not be read") from error
    if count!=spec["bytes"] or digest.hexdigest()!=spec["sha256"]:
        raise RangeUnavailable("Changed member did not match its signed length and hash")
    output.seek(0)
