"""Measured download progress and bounded recovery of interrupted targets.

Progress packets omit URLs and credentials. Final HTTP errors identify only
the public server and basename, never URL user information or query values.
"""
import time
import uuid
from pathlib import PurePosixPath
from urllib.parse import urlsplit,urlunsplit
from urllib3 import Timeout
from urllib3.exceptions import (
    MaxRetryError, NewConnectionError, ProtocolError, SSLError,
    TimeoutError as HttpTimeoutError,
)
from urllib3.util import Retry
from tuf.api.exceptions import DownloadError, DownloadHTTPError
from tuf.ngclient.fetcher import FetcherInterface
from tuf.ngclient.urllib3_fetcher import Urllib3Fetcher

_sink=None
_last={}
def configure(sink):
 global _sink,_last
 _sink=sink;_last={}
def report(stage,component='',completed=0,total=0,*,force=False):
 if _sink is None:return
 if stage not in ('metadata','download','verify','prepare','drain','startup','committed'):raise ValueError('Unknown update progress stage')
 if component not in ('','ui','core','gpu','cpu'):raise ValueError('Unknown progress component')
 if type(completed)is not int or type(total)is not int or not 0<=completed<=total<2**53:raise ValueError('Invalid measured progress')
 key=(stage,component);now=time.monotonic()
 if not force and completed!=total and now-_last.get(key,-1)<.15:return
 _last[key]=now
 _sink(dict(schema='PGN_UPDATE_PROGRESS_V1',stage=stage,component=component,completed=completed,total=total))

_GATEWAY_ERRORS = {500, 502, 503, 504}
_TARGET_RETRY_ERRORS = _GATEWAY_ERRORS | {408, 429}
_RETRY_DELAYS = (0.5, 2, 8)
_TRANSIENT_CONNECTION_ERRORS = (HttpTimeoutError, NewConnectionError, ProtocolError)


def _fresh_url(url):
    parsed = urlsplit(url)
    if (parsed.scheme == 'https'
            and parsed.hostname in ('github.com', 'raw.githubusercontent.com')
            and not parsed.query):
        return urlunsplit((parsed.scheme, parsed.netloc, parsed.path,
                           'pgn_retry=' + uuid.uuid4().hex, ''))
    return url


def _public_file(url):
    parsed = urlsplit(url)
    return parsed.hostname or 'update server', PurePosixPath(parsed.path).name[:180]


class ResumableTargetFetcher(Urllib3Fetcher):
    """Use the existing TUF transport's proxy and TLS settings for byte ranges."""

    def fetch_target(self, url, offset, length):
        # Keep redirect handling, but own connection/read retries so the
        # bounded retry loop can resume at the last byte actually delivered.
        redirects = Retry(total=5, redirect=5, connect=0, read=0, status=0,
                          respect_retry_after_header=False)
        headers = {'Accept-Encoding': 'identity', 'User-Agent': 'PGN-Horizon-Updater'}
        if offset:
            headers['Range'] = f'bytes={offset}-{length - 1}'
        while True:
            response = self._proxy_env.request(
                'GET', url, headers=headers, preload_content=False,
                timeout=Timeout(self.socket_timeout), retries=redirects,
            )
            try:
                if response.status == 416 and 'Range' in headers:
                    # A host rejecting Range can still provide the full
                    # representation. Feed TUF only the suffix it needs.
                    del headers['Range']
                    continue
                if response.status >= 400:
                    raise DownloadHTTPError('Update download failed', response.status)
                if response.headers.get('Content-Encoding', 'identity').lower() != 'identity':
                    raise DownloadError('The update server sent an unexpected content encoding.')
                discard = offset
                if response.status == 206:
                    expected = f'bytes {offset}-{length - 1}/{length}'
                    if 'Range' not in headers or response.headers.get('Content-Range') != expected:
                        raise DownloadError('The update server sent an invalid byte range.')
                    discard = 0
                elif response.status != 200:
                    raise DownloadError('The update server did not return the requested file.')
                for chunk in response.stream(self.chunk_size, decode_content=False):
                    skipped = min(discard, len(chunk))
                    discard -= skipped
                    if skipped < len(chunk):
                        yield chunk[skipped:]
                return
            finally:
                response.close()
                response.release_conn()


class ProgressFetcher(FetcherInterface):
    def __init__(self, inner):
        self.inner = inner
        self.target_length = 0

    def _fetch_target(self, url, fetch, length):
        host, name = _public_file(url)
        current = url
        received = 0
        for delay in (*_RETRY_DELAYS, None):
            try:
                for chunk in fetch(current, received, length):
                    if received + len(chunk) > length:
                        raise DownloadError('The update download exceeded its signed length.')
                    received += len(chunk)
                    report('download', completed=received, total=length)
                    yield chunk
                if received == length:
                    return
                failure = DownloadError('The connection closed before all update bytes arrived.')
            except DownloadHTTPError as error:
                if error.status_code not in _TARGET_RETRY_ERRORS:
                    raise DownloadHTTPError(
                        f'Update server {host} returned HTTP {error.status_code} for {name}.',
                        error.status_code,
                    ) from error
                failure = error
            except _TRANSIENT_CONNECTION_ERRORS as error:
                failure = error
            except MaxRetryError as error:
                if not isinstance(error.reason, _TRANSIENT_CONNECTION_ERRORS):
                    raise DownloadError(f'Could not connect to the update server {host}.') from error
                failure = error
            except SSLError as error:
                raise DownloadError(f'Could not establish a secure connection to {host}.') from error
            if received == length:
                # The reference TUF client still checks the complete signed
                # bytes when a connection goes away after the final chunk.
                return
            if delay is None:
                raise DownloadError(
                    f'The connection to {host} was interrupted while downloading {name}. Try downloading again.'
                ) from failure
            time.sleep(delay)
            current = _fresh_url(url)

    def _fetch(self, url):
        target_fetch = getattr(self.inner, 'fetch_target', None)
        if self.target_length and target_fetch is not None:
            yield from self._fetch_target(url, target_fetch, self.target_length)
            return
        count = 0
        current = url
        for attempt in range(2):
            try:
                for chunk in self.inner.fetch(current):
                    count += len(chunk)
                    if self.target_length:
                        report('download', completed=min(count, self.target_length), total=self.target_length)
                    yield chunk
                return
            except DownloadHTTPError as error:
                if attempt == 0 and count == 0 and error.status_code in _GATEWAY_ERRORS:
                    current = _fresh_url(url)
                    time.sleep(0.5)
                    continue
                host, name = _public_file(url)
                raise DownloadHTTPError(
                    f'Update server {host} returned HTTP {error.status_code} while fetching {name}; retry later.',
                    error.status_code,
                ) from error
