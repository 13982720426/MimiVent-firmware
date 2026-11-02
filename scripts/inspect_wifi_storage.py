#!/usr/bin/env python3
"""默认只读检查旧实验身份。删除必须显式给出设备身份、pin 和两个确认参数。"""
import argparse,asyncio,json,struct,sys

async def run(args):
 sys.path.insert(0,str(args.agent_root))
 from mimivent_agent.client import BleSession
 from mimivent_agent.protocol import Decoder
 class Session(BleSession):
  async def connect(self,device):
   async with asyncio.timeout(35):
    await super().connect(device)
    await self.client.start_notify('6f9b0012-7d5a-4c3b-9a21-2f6d4d494d49',self._notification)
    await asyncio.sleep(2)
  async def requestStorage(self,op,**fields):
   rid=self.next_id;self.next_id+=1;self.decoder=Decoder(rid)
   self.pending=asyncio.get_running_loop().create_future()
   try:
    async with asyncio.timeout(20):
     await self.client.write_gatt_char('6f9b0011-7d5a-4c3b-9a21-2f6d4d494d49',struct.pack('<H',rid)+json.dumps({'op':op,**fields}).encode(),response=True)
     code,raw=await self.pending
     if code!=200:raise RuntimeError('BLE_RESPONSE')
     return json.loads(raw)
   finally:self.pending=None
 session=Session()
 try:
  await session.connect(args.device)
  before=await session.requestStorage('wifi.storage.legacyStatus')
  if before.get('error'):raise RuntimeError(before['error'])
  if args.list_clients:
   print(json.dumps(await session.requestStorage('wifi.clients'),ensure_ascii=False),flush=True)
  if args.check_control_status:
   state=await session.request(path='/api/status')
   print(json.dumps({'bleStatusReadable':True,'firmware':state.get('firmware'),'mode':state.get('mode')},ensure_ascii=False),flush=True)
   ota=await session.ota_command('QUERY',['IDLE','PAUSED','VERIFIED','ERROR'],allow_error=True)
   print(json.dumps({'otaState':ota['raw'].split('|',1)[0]}),flush=True)
  if args.abandon_test_package:
   digest=__import__('hashlib').sha256(args.abandon_test_package.read_bytes()).hexdigest()
   raw=(await session.ota_command('QUERY',['IDLE','PAUSED','ERROR','VERIFIED'],allow_error=True))['raw']
   parts=raw.split('|')
   if raw!='IDLE':
    if len(parts)<8 or parts[0]!='PAUSED' or parts[7]!=digest:raise RuntimeError('PRESERVE_UNRELATED_OTA_TASK')
    cleared=await session.ota_command('ABANDON|'+parts[5]+'|9',['IDLE'])
    if cleared['raw']!='IDLE':raise RuntimeError('TEST_TASK_CLEANUP_FAILED')
   print(json.dumps({'testOtaTaskCleared':True}),flush=True)
  for client in args.revoke_test_client:
   if not __import__('re').fullmatch(r'probe-[ab]-[0-9a-f]{12}',client):raise RuntimeError('NOT_A_TEST_CLIENT')
   current=await session.requestStorage('wifi.clients')
   for item in current.get('clients',[]):
    if item.get('clientId')==client:
     revoked=await session.requestStorage('wifi.revoke',tokenId=item['tokenId'])
     if revoked.get('error'):raise RuntimeError(revoked['error'])
   print(json.dumps({'revokedTestClient':client}),flush=True)
  if args.delete_legacy_identities:
   if not args.confirm or not args.expected_device_id or not args.expected_pin:raise RuntimeError('EXPLICIT_CONFIRMATION_REQUIRED')
   result=await session.requestStorage('wifi.storage.cleanupLegacy',expectedDeviceId=args.expected_device_id,expectedCertPin=args.expected_pin,confirm='DELETE_LEGACY_WIFI_IDENTITIES')
   if result.get('error'):raise RuntimeError(result['error'])
   print(json.dumps(result,ensure_ascii=False),flush=True)
  else:
   # 验证缺少确认时拒绝，不尝试正确的删除确认。
   rejected=await session.requestStorage('wifi.storage.cleanupLegacy',expectedDeviceId=before['deviceId'],expectedCertPin=before['certPinSha256'])
   if rejected.get('error')!='CONFIRMATION_REQUIRED':raise RuntimeError('UNEXPECTED_CONFIRMATION_RESULT')
   after=await session.requestStorage('wifi.storage.legacyStatus')
   if before['legacyIdentityKeys']!=after['legacyIdentityKeys']:raise RuntimeError('READ_ONLY_INSPECTION_CHANGED_KEYS')
   checks={'inventory':after,'missingConfirmationRejected':True}
   if args.check_storage_gate:
    if after.get('nvsFreeEntries',999)>=207:raise RuntimeError('LOW_STORAGE_PRECONDITION_NOT_MET')
    clients=await session.requestStorage('wifi.clients')
    opened=await session.requestStorage('wifi.bind.open')
    if opened.get('error'):raise RuntimeError(opened['error'])
    attempt=await session.requestStorage('wifi.bind.begin',clientId='storage-gate-probe',requestId=str(__import__('uuid').uuid4()))
    if attempt.get('error')!='WIFI_STORAGE_LOW':
     await session.requestStorage('wifi.bind.cancel',tokenId=attempt.get('tokenId',''))
     raise RuntimeError('STORAGE_GATE_NOT_ENFORCED')
    final=await session.requestStorage('wifi.storage.legacyStatus')
    if final['nvsFreeEntries']!=after['nvsFreeEntries'] or await session.requestStorage('wifi.clients')!=clients:raise RuntimeError('STORAGE_GATE_CHANGED_PERSISTENT_STATE')
    checks['lowStorageRejectedWithoutWrites']=True
   print(json.dumps(checks,ensure_ascii=False),flush=True)
 finally:await session.disconnect()

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--agent-root',type=__import__('pathlib').Path,required=True);p.add_argument('--device',required=True)
 p.add_argument('--delete-legacy-identities',action='store_true');p.add_argument('--expected-device-id');p.add_argument('--expected-pin');p.add_argument('--confirm',action='store_true')
 p.add_argument('--check-storage-gate',action='store_true',help='仅在 free entries < 207 时验证低容量拒绝，不保存绑定')
 p.add_argument('--list-clients',action='store_true',help='只显示客户端 ID 与 token ID，不显示凭据')
 p.add_argument('--check-control-status',action='store_true',help='只读检查普通 BLE 状态和 OTA 阶段')
 p.add_argument('--revoke-test-client',action='append',default=[],help='仅撤销明确指定的 probe-a/b-随机ID 测试客户端')
 p.add_argument('--abandon-test-package',type=__import__('pathlib').Path,help='仅在包 SHA-256 完全匹配时放弃本轮测试暂停任务')
 asyncio.run(run(p.parse_args()))
