#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host integration checks; never contacts a console or modifies game files."""
import os,pathlib,json,threading,subprocess,time,urllib.request,urllib.error,http.server,socket,tempfile
root=pathlib.Path(__file__).resolve().parent.parent
fixture=tempfile.TemporaryDirectory(prefix='lz4-api-source-')
calls=[]
drive=tempfile.TemporaryDirectory(prefix='lz4-test-drive-')
dump=pathlib.Path(drive.name)/'homebrew/PPSA00002-app'
for name,data in {'eboot.bin':b'Folder executable','sce_sys/param.json':json.dumps({'titleId':'PPSA00002','localizedParameters':{'defaultLanguage':'en-US','en-US':{'titleName':'Folder fixture'}}}).encode(),'assets/repeated':b'folder-block'*40000,'assets/random':os.urandom(100000)}.items():
    (dump/name).parent.mkdir(parents=True,exist_ok=True);(dump/name).write_bytes(data)
game={'title_id':'PPSA00001','title_name':'Synthetic fixture','path':fixture.name+'/fixture.ffpfsc','runtime_path':fixture.name,'managed':True,'source_available':True,'mounted':True,'image_backed':True}
class Shadow(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        calls.append((self.path,body))
        data=json.dumps({'status':0,'games':[game]} if self.path.endswith('/games') else {'status':0}).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def log_message(self,*args):pass
mock=http.server.ThreadingHTTPServer(('127.0.0.1',10102),Shadow)
threading.Thread(target=mock.serve_forever,daemon=True).start()
service=subprocess.Popen([str(root/'build/host-service')],env=dict(os.environ,STUDIO_TEST_DRIVE=drive.name),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
def request(path,method='GET'):
    req=urllib.request.Request('http://127.0.0.1:15911'+path,method=method)
    try:
        with urllib.request.urlopen(req,timeout=10) as r:return r.status,r.read()
    except urllib.error.HTTPError as e:return e.code,e.read()
def api(path,method='GET',ok=True):
    code,data=request(path,method);j=json.loads(data);assert j['ok']==ok,(code,j);assert (code==200)==ok,(code,j);return j
try:
    for _ in range(100):
        try:api('/api/gc/ampr-lz4/status');break
        except (OSError,urllib.error.URLError):time.sleep(.05)
    else:raise AssertionError('Service did not start')
    assert request('/')[0]==200
    assert b'Controller' in request('/')[1]
    assert b'beta.1' in request('/')[1] and b'Conversion options' in request('/')[1]
    assert api('/api/gc/games')['games'][0]['titleId']=='PPSA00001'
    query=urllib.parse.urlencode({'titleId':'PPSA00001','sourcePath':game['path'],'output':'/data/homebrew/fixture-output','level':'7','mode':'internal'})
    p=api('/api/gc/ampr-lz4/plan?'+query);assert not p['canStart'] and not p['readOnlyVerified']
    api('/api/gc/ampr-lz4/plan?'+query+'&verifyOutput=invalid',ok=False)
    api('/api/gc/ampr-lz4/plan?'+query+'&originalAction=invalid',ok=False)
    api('/api/gc/ampr-lz4/plan?'+query+'&originalAction=swap',ok=False)
    api('/api/gc/ampr-lz4/plan?'+query.replace('mode=internal','mode=swap')+'&originalAction=swap&verifyCopy=no',ok=False)
    api('/api/gc/ampr-lz4/start?'+query,'POST',False)
    api('/api/gc/ampr-lz4/mount?'+query,'GET',False)
    api('/api/gc/ampr-lz4/mount?'+query,'POST')
    assert [x[0] for x in calls[-2:]]==['/api/v1/games/unmount','/api/v1/games/mount']
    assert calls[-1][1]['mode']=='ro'
    settings=api('/api/gc/ampr-lz4/threads');assert settings['threadLimit']==8
    api('/api/gc/ampr-lz4/threads?packReaders=2&packWorkers=4&packWriters=2','POST',False)
    api('/api/gc/ampr-lz4/threads?packReaders=1&packWorkers=4&packWriters=2','POST')
    assert api('/api/gc/ampr-lz4/threads')['packing']['workers']==4
    api('/api/gc/ampr-lz4/threads?packReaders=1&packReaders=2','POST',False)
    api('/api/gc/ampr-lz4/threads?packReaders=%00','POST',False)
    api('/api/gc/job/cancel','GET',False)
    api('/api/gc/job/cancel','POST')
    assert not api('/api/gc/ampr-lz4/status')['active']
    assert request('/ampr-runtime.sprx')[0]==404
    assert request('/../src/ampr_backend.cpp')[0]==404
    assert request('/api/nonexistent')[0]==400
    api('/api/gc/ampr-lz4/delete-partial','GET',False)
    api('/api/gc/ampr-lz4/delete-partial','POST')
    folders=[g for g in api('/api/gc/games')['games'] if g['sourceKind']=='folder']
    assert len(folders)==1 and folders[0]['titleId']=='PPSA00002' and folders[0]['name']=='Folder fixture' and folders[0]['readOnlyVerified'] and folders[0]['sourceStorage']=='Test drive',folders
    output=pathlib.Path(drive.name)/'homebrew/PPSA00002-app_AMPR'
    folderQuery=urllib.parse.urlencode({'titleId':'PPSA00002','sourcePath':folders[0]['sourcePath'],'output':str(output),'level':'7','mode':'usb'})
    p=api('/api/gc/ampr-lz4/plan?'+folderQuery);assert p['readOnlyVerified'] and p['canStart'] and p['sourceKind']=='folder',p
    for action in ('delete','move'):api('/api/gc/ampr-lz4/plan?'+folderQuery+'&originalAction='+action,ok=False)
    api('/api/gc/ampr-lz4/plan?'+folderQuery.replace('mode=usb','mode=swap')+'&originalAction=swap',ok=False)
    before=len(calls);assert api('/api/gc/ampr-lz4/mount?'+folderQuery,'POST')['message']=='Folder dumps need no mount' and len(calls)==before
    original={f:f.read_bytes() for f in dump.rglob('*') if f.is_file()}
    api('/api/gc/ampr-lz4/start?'+folderQuery,'POST')
    for _ in range(600):
        state=api('/api/gc/ampr-lz4/status')
        if not state['active']:break
        time.sleep(.1)
    assert state['result']==0 and state['verified'] and state['phase']=='Complete: output verified',state
    assert (output/'ampr_assets.index').is_file() and not (output/'INCOMPLETE.txt').exists()
    assert original=={f:f.read_bytes() for f in dump.rglob('*') if f.is_file()}
    assert [g['sourcePath'] for g in api('/api/gc/games')['games'] if g['sourceKind']=='folder']==[str(dump)]
    mounts=api('/api/gc/ampr-lz4/mounts')
    assert [d['path'] for d in mounts['drives']][:2]==['/mnt/ext1','/mnt/ext0'] and mounts['drives'][0]['name']=='M.2 SSD' and isinstance(mounts['mounts'],list)
    api('/api/control/quit?confirm=yes','GET',False)
    api('/api/control/quit','POST',False)
    assert b'quitDialog' in request('/')[1]
    assert api('/api/control/quit?confirm=yes','POST')['message']=='Quitting'
    service.wait(timeout=5);assert service.returncode==0
    print('PASS: service assets, discovery, mount order, read-only gate, worker limit, method checks, malformed queries, cleanup, status, folder-dump conversion and quit')
finally:
    service.terminate();service.wait(timeout=5);mock.shutdown();fixture.cleanup();drive.cleanup()
