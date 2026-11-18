import hashlib
import socket
import sys,unittest
import threading
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock
from urllib.parse import urlsplit
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src/update'))
from tuf.api.exceptions import DownloadError, DownloadHTTPError, LengthOrHashMismatchError
from tuf.api.metadata import TargetFile
import horizon_update_progress as progress

class ScriptedFetcher:
 def __init__(self,*responses):self.responses=list(responses);self.urls=[]
 def fetch(self,url):
  self.urls.append(url);response=self.responses.pop(0)
  for value in response:
   if isinstance(value,Exception):raise value
   yield value

class RecoveryTests(unittest.TestCase):
 def tearDown(self):progress.configure(None)
 @mock.patch.object(progress.time,'sleep')
 def test_gateway_failure_recovers_without_changing_body(self,sleep):
  inner=ScriptedFetcher([DownloadHTTPError('gateway',504)],[b'ab',b'c'])
  client=progress.ProgressFetcher(inner);client.target_length=3;events=[];progress.configure(events.append)
  self.assertEqual(b''.join(client.fetch('https://github.com/owner/repo/file.zip')),b'abc')
  self.assertEqual(len(inner.urls),2);a,b=map(urlsplit,inner.urls)
  self.assertEqual((a.scheme,a.netloc,a.path),(b.scheme,b.netloc,b.path))
  self.assertTrue(b.query.startswith('pgn_retry='));self.assertEqual(events[-1]['completed'],3)
  sleep.assert_called_once_with(.5)
 @mock.patch.object(progress.time,'sleep')
 def test_partial_response_is_never_replayed(self,sleep):
  inner=ScriptedFetcher([b'first',DownloadHTTPError('gateway',504)],[b'duplicate'])
  stream=progress.ProgressFetcher(inner).fetch('https://github.com/owner/file.zip')
  self.assertEqual(next(stream),b'first')
  with self.assertRaises(DownloadHTTPError)as caught:next(stream)
  self.assertEqual(caught.exception.status_code,504);self.assertEqual(len(inner.urls),1);sleep.assert_not_called()
 @mock.patch.object(progress.time,'sleep')
 def test_expected_missing_root_does_not_retry(self,sleep):
  inner=ScriptedFetcher([DownloadHTTPError('missing',404)])
  with self.assertRaises(DownloadHTTPError)as caught:b''.join(progress.ProgressFetcher(inner).fetch('https://raw.githubusercontent.com/owner/2.root.json'))
  self.assertEqual(caught.exception.status_code,404);self.assertEqual(len(inner.urls),1);sleep.assert_not_called()
 @mock.patch.object(progress.time,'sleep')
 def test_repeated_failure_is_bounded_and_does_not_expose_secrets(self,sleep):
  inner=ScriptedFetcher([DownloadHTTPError('temporary',503)],[DownloadHTTPError('still unavailable',502)])
  with self.assertRaises(DownloadHTTPError)as caught:b''.join(progress.ProgressFetcher(inner).fetch('https://user:secret@github.com/owner/file.zip?token=private'))
  self.assertEqual(len(inner.urls),2);self.assertEqual(caught.exception.status_code,502)
  message=str(caught.exception);self.assertIn('github.com',message);self.assertIn('file.zip',message)
  for secret in ('user:','secret','private','token='):self.assertNotIn(secret,message)
  self.assertEqual(inner.urls[0],inner.urls[1]);sleep.assert_called_once()

class ResumedTargetTests(unittest.TestCase):
    body = b'1234567890abcdef'
    partial = 6

    def setUp(self):
        self.expected = TargetFile(
            len(self.body), {'sha256': hashlib.sha256(self.body).hexdigest()}, 'update.zip'
        )
        self.fetcher = progress.ProgressFetcher(
            progress.ResumableTargetFetcher(socket_timeout=2, chunk_size=2)
        )
        self.fetcher.target_length = len(self.body)
        self.reports = []
        progress.configure(self.reports.append)
        delays = mock.patch.object(progress, '_RETRY_DELAYS', (0, 0, 0))
        delays.start()
        self.addCleanup(delays.stop)
        self.addCleanup(progress.configure, None)

    @contextmanager
    def server(self, responses):
        requests = []
        index = iter(responses)
        body = self.body
        cutoff = self.partial

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                requests.append(self.headers.get('Range'))
                mode = next(index, 'unexpected')
                if mode in ('gateway', 'range_refused', 'unexpected'):
                    status = {'gateway': 503, 'range_refused': 416, 'unexpected': 500}[mode]
                    self.send_response(status)
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                    return
                content = body
                if mode in ('suffix', 'wrong_range', 'corrupt_suffix'):
                    self.send_response(206)
                    start = cutoff + (mode == 'wrong_range')
                    self.send_header('Content-Range', f'bytes {start}-{len(body) - 1}/{len(body)}')
                    content = body[cutoff:]
                    if mode == 'corrupt_suffix':
                        content = b'x' + content[1:]
                else:
                    self.send_response(200)
                self.send_header('Content-Length', str(len(content)))
                self.end_headers()
                if mode == 'cut':
                    self.wfile.write(content[:cutoff])
                    self.wfile.flush()
                    self.connection.shutdown(socket.SHUT_WR)
                else:
                    try:
                        self.wfile.write(content)
                    except (BrokenPipeError, ConnectionResetError):
                        pass

        with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
            worker = threading.Thread(target=server.serve_forever, args=(0.02,), daemon=True)
            worker.start()
            try:
                yield f'http://127.0.0.1:{server.server_port}/update.zip', requests
            finally:
                server.shutdown()
                worker.join()

    def download(self, url):
        with self.fetcher.download_file(url, self.expected.length) as downloaded:
            self.expected.verify_length_and_hashes(downloaded)
            downloaded.seek(0)
            return downloaded.read()

    def test_gateway_and_real_socket_cut_resume_once_with_monotonic_progress(self):
        with self.server(['gateway', 'cut', 'suffix']) as (url, requests):
            self.assertEqual(self.download(url), self.body)
        self.assertEqual(requests, [None, None, 'bytes=6-15'])
        counts = [packet['completed'] for packet in self.reports]
        self.assertEqual(counts, sorted(set(counts)))
        self.assertEqual(counts[-1], len(self.body))

    def test_server_ignoring_range_does_not_duplicate_prefix(self):
        with self.server(['cut', 'full']) as (url, requests):
            self.assertEqual(self.download(url), self.body)
        self.assertEqual(requests, [None, 'bytes=6-15'])

    def test_server_refusing_range_falls_back_to_full_response(self):
        with self.server(['cut', 'range_refused', 'full']) as (url, requests):
            self.assertEqual(self.download(url), self.body)
        self.assertEqual(requests, [None, 'bytes=6-15', None])

    def test_inconsistent_range_is_rejected(self):
        with self.server(['cut', 'wrong_range']) as (url, requests):
            with self.assertRaisesRegex(DownloadError, 'invalid byte range'):
                self.download(url)
        self.assertEqual(requests, [None, 'bytes=6-15'])

    def test_resumed_wrong_bytes_fail_the_tuf_target_hash(self):
        with self.server(['cut', 'corrupt_suffix']) as (url, _):
            with self.assertRaises(LengthOrHashMismatchError):
                self.download(url)

    def test_four_unsuccessful_requests_leave_the_next_download_usable(self):
        with self.server(['cut', 'gateway', 'gateway', 'gateway', 'full']) as (url, requests):
            with self.assertRaisesRegex(DownloadError, 'Try downloading again'):
                self.download(url)
            self.assertEqual(self.download(url), self.body)
        self.assertEqual(requests, [None, *(['bytes=6-15'] * 3), None])


if __name__=='__main__':unittest.main()
