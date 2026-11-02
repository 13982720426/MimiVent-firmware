#!/usr/bin/env python3
"""安全绑定真机联调；凭据只驻内存，Dart 探针通过 stdin 接收，测试后撤销。"""
import argparse, asyncio, hashlib, importlib.util, json, struct, sys, time, uuid
from pathlib import Path
from run_p3_ota_regression import write_data

async def run(args):
 sys.path.insert(0,str(args.agent_root))
 from mimivent_agent.client import BleSession
 from mimivent_agent.protocol import Decoder,OTA_DATA
 from mimivent_agent.ota import OtaService
 command='6f9b0011-7d5a-4c3b-9a21-2f6d4d494d49'
 response='6f9b0012-7d5a-4c3b-9a21-2f6d4d494d49'
 class Session(BleSession):
  async def connect(self,device):
   async with asyncio.timeout(35):
    await super().connect(device)
    await self.client.start_notify(response,self._notification)
    await asyncio.sleep(2)
  async def write_ota_data(self,frame):await write_data(self.connected(),frame,OTA_DATA)
  async def secure(self,op,**fields):
   async with self.lock:
    rid=self.next_id;self.next_id=self.next_id%65535+1
    self.decoder=Decoder(rid);self.pending=asyncio.get_running_loop().create_future()
    try:
     async with asyncio.timeout(30):
      await self.client.write_gatt_char(command,struct.pack('<H',rid)+json.dumps({'op':op,**fields},separators=(',',':')).encode(),response=True)
      code,body=await self.pending;value=json.loads(body)
      if code!=200:raise RuntimeError('BLE_STATUS')
      return value
    finally:self.pending=None
 session=Session();steps=[];created=[];ota=None
 def record(step):
  steps.append(step);args.output.parent.mkdir(parents=True,exist_ok=True)
  args.output.write_text(json.dumps({'steps':steps},ensure_ascii=False,indent=2))
  print(step,flush=True)
 async def call(op,**fields):
  value=await session.secure(op,**fields)
  if value.get('error'):raise RuntimeError(value['error'])
  return value
 async def probe(binding,expected='active'):
  # 不经 shell，也不把 token 放在 argv 或日志中。
  process=await asyncio.create_subprocess_exec(str(args.dart),'run','tools/wifi_read_probe.dart',cwd=str(args.app_root),stdin=asyncio.subprocess.PIPE,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE)
  raw,_=await asyncio.wait_for(process.communicate(json.dumps(binding).encode()),30)
  result=json.loads(raw)
  if expected.startswith('error:'):
   if result.get('error')!=expected.split(':',1)[1]:raise RuntimeError('UNEXPECTED_AUTH_RESULT')
  elif process.returncode or result.get('bindingState')!=expected:raise RuntimeError(result.get('error','READ_FAILED'))
  return result
 async def begin(client,replace=False):
  await call('wifi.bind.open');request=str(uuid.uuid4())
  value=await call('wifi.bind.begin',clientId=client,requestId=request,replace=replace)
  again=await call('wifi.bind.begin',clientId=client,requestId=request,replace=replace)
  if value['token']!=again['token'] or value['tokenId']!=again['tokenId']:raise RuntimeError('BEGIN_NOT_IDEMPOTENT')
  binding={**value,'version':1,'clientId':client}
  created.append(binding);return binding
 async def commit(binding):
  await probe(binding,'pending')
  result=await call('wifi.bind.commit',tokenId=binding['tokenId'])
  if result.get('bindingState')!='active':raise RuntimeError('COMMIT_FAILED')
  await call('wifi.bind.commit',tokenId=binding['tokenId'])
  binding['bindingState']='active';await asyncio.sleep(.55);await probe(binding)
 try:
  await session.connect(args.device)
  for _ in range(15):
   status=await call('wifi.status')
   if status.get('wifiReadAvailable'):break
   await asyncio.sleep(1)
  else:raise RuntimeError('WIFI_READ_UNAVAILABLE')
  clients=await call('wifi.clients')
  if clients.get('clients'):raise RuntimeError('PRESERVE_EXISTING_CLIENTS')
  suffix=uuid.uuid4().hex[:12];clientA='probe-a-'+suffix;clientB='probe-b-'+suffix
  a=await begin(clientA);await commit(a);record('client-a-active')
  b=await begin(clientB);await commit(b);record('client-b-active')
  await call('wifi.bind.open')
  full=await session.secure('wifi.bind.begin',clientId='probe-third-'+suffix,requestId=str(uuid.uuid4()))
  if full.get('error')!='BINDINGS_FULL':raise RuntimeError('CAPACITY_LIMIT_FAILED')
  record('third-client-rejected')
  candidate=await begin(clientA,True)
  await probe(a) # 候选提交前旧 token 仍有效。
  await commit(candidate);await probe(a,'error:BINDING_INVALID');a=candidate
  await probe(b);record('replacement-atomic-other-client-preserved')
  # 生成真实 OTA 双槽检查点；仅暂停保留任务，不提交安装。
  q=(await session.ota_command('QUERY',['IDLE','PAUSED','VERIFIED','INSTALLED','ERROR'],allow_error=True))['raw']
  if q!='IDLE':raise RuntimeError('PRESERVE_EXISTING_OTA_TASK')
  ota=OtaService(session)
  await ota.call('ota.upload',{'confirm':True,'filePath':str(args.package)})
  deadline=time.monotonic()+60
  while ota.busy and ota.sent<65536:
   if time.monotonic()>deadline:raise RuntimeError('OTA_TEST_TIMEOUT')
   await asyncio.sleep(.1)
  ota._pause_requested=True
  await asyncio.wait_for(ota.task,30)
  if ota.phase!='paused' or ota.error:raise RuntimeError('OTA_PAUSE_FAILED:'+str(ota.error))
  record('ota-checkpoint-with-two-clients')
  for _ in range(30):
   network=await call('wifi.status')
   if network.get('wifiReadAvailable'):break
   await asyncio.sleep(1)
  else:raise RuntimeError('WIFI_DID_NOT_RESUME')
  a['host']=b['host']=network['ip']
  # 反复替换，触发 NVS 页回收；OTA 双槽记录同时存在。
  for index in range(args.rotations):
   candidate=await begin(clientA,True);await commit(candidate)
   await probe(a,'error:BINDING_INVALID');a=candidate
   record('nvs-replacement-'+str(index+1))
  await session.disconnect();await probe(a);await probe(b)
  record('wifi-read-with-ble-disconnected')
  record('WAIT_REBOOT')
  deadline=time.monotonic()+180
  while not args.reboot_marker.exists():
   if time.monotonic()>deadline:raise RuntimeError('REBOOT_MARKER_TIMEOUT')
   await asyncio.sleep(.5)
  await asyncio.sleep(8)
  await session.connect(args.device)
  for _ in range(20):
   status=await call('wifi.status')
   if status.get('wifiReadAvailable'):break
   await asyncio.sleep(1)
  if status.get('deviceId')!=a['deviceId']:raise RuntimeError('IDENTITY_CHANGED')
  a['host']=b['host']=status['ip']
  await probe(a);await probe(b);record('two-clients-survive-reboot')
  q=(await session.ota_command('QUERY',['PAUSED','ERROR'],allow_error=True))['raw'].split('|')
  if len(q)<8 or q[5]!=ota.selection['taskId']:raise RuntimeError('CHECKPOINT_LOST')
  await session.ota_command('ABANDON|'+q[5]+'|9',['IDLE'])
  record('ota-checkpoint-survives-reboot-and-clears')
  await call('wifi.revoke',tokenId=a['tokenId']);await call('wifi.revoke',tokenId=a['tokenId'])
  await probe(a,'error:BINDING_INVALID');await probe(b)
  await call('wifi.revoke',tokenId=b['tokenId']);await probe(b,'error:BINDING_INVALID')
  pending=await begin('probe-cancel-'+suffix)
  await call('wifi.bind.cancel',tokenId=pending['tokenId']);await call('wifi.bind.cancel',tokenId=pending['tokenId'])
  await probe(pending,'error:BINDING_INVALID')
  if (await call('wifi.clients'))['clients']:raise RuntimeError('TEST_CLIENT_CLEANUP_FAILED')
  record('revoke-and-cancel-idempotent-all-test-clients-cleared')
 except Exception as e:
  # 仅输出受控错误类型/code，绝不输出 binding 字典或请求正文。
  record('FAILED:'+type(e).__name__+(':'+str(e) if isinstance(e,RuntimeError) else ''))
  raise
 finally:
  if ota:await ota.close()
  # 异常时也尽力清理本次生成的测试客户端；绝不操作预先存在的绑定。
  try:
   if created and (session.client is None or not session.client.is_connected):await session.connect(args.device)
   for binding in created:
    await session.secure('wifi.bind.cancel',tokenId=binding['tokenId'])
    await session.secure('wifi.revoke',tokenId=binding['tokenId'])
   if ota and ota.selection:
    q=(await session.ota_command('QUERY',['IDLE','PAUSED','VERIFIED','ERROR'],allow_error=True))['raw']
    if ota.selection['taskId'] in q:
     await session.ota_command('ABANDON|'+ota.selection['taskId']+'|9',['IDLE','ERROR'],allow_error=True)
  except Exception:
   record('cleanup-requires-inspection')
  await session.disconnect()

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--agent-root',type=Path,required=True);p.add_argument('--app-root',type=Path,required=True)
 p.add_argument('--dart',type=Path,required=True);p.add_argument('--device',required=True)
 p.add_argument('--package',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--reboot-marker',type=Path,required=True);p.add_argument('--rotations',type=int,default=6)
 asyncio.run(run(p.parse_args()))
