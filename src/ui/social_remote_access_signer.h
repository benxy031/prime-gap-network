#pragma once
#include "social/social_access_signing_contract.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <memory>

namespace pgn::ui {
// Only the selected mining wallet may sign its own access command.
// This reuses the configured SSH identity, keeps its RPC cookie on that device,
// and neither starts mining nor exposes a generic remote RPC/signing interface.
struct RemoteAccessConsentPlan {
 QString program;QStringList arguments;QByteArray input,profileHash;QString error;
};
inline QByteArray nativeAccessSignerPowerShell(){return QByteArray(R"PS(
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::InputEncoding=New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false)
$pgnReason='The mining wallet connection is unavailable.'
try {
 $pgnLine=[Console]::In.ReadLine()
 if($null -eq $pgnLine -or $pgnLine.Length -gt 16000){throw 'frame'}
 $pgnRequest=$pgnLine|ConvertFrom-Json
 if($pgnRequest.genesis -notmatch '^[0-9a-f]{64}$' -or $pgnRequest.script -notmatch '^0014[0-9a-f]{40}$' -or $pgnRequest.rpc_port -lt 1 -or $pgnRequest.rpc_port -gt 65535){throw 'scope'}
 $pgnDomain="PGN/SOCIAL/ACCESS/1`n"
 if(-not $pgnRequest.challenge.StartsWith($pgnDomain)){throw 'purpose'}
 $pgnCommand=$pgnRequest.challenge.Substring($pgnDomain.Length)|ConvertFrom-Json
 $pgnFields=(@($pgnCommand.PSObject.Properties.Name)|Sort-Object)-join ','
 if($pgnFields -cne 'action,actor,address,args,client,community,created_ms,genesis,request_id,revision,v'){throw 'fields'}
 if($pgnRequest.client -notmatch '^0014[0-9a-f]{40}$' -or $pgnRequest.community -notmatch '^[0-9a-f]{64}$'){throw 'client'}
 if($pgnCommand.v -ne 1 -or $pgnCommand.genesis -cne $pgnRequest.genesis -or $pgnCommand.actor -cne $pgnRequest.script -or $pgnCommand.client -cne $pgnRequest.client -or $pgnCommand.community -cne $pgnRequest.community -or $pgnCommand.address -cne $pgnRequest.miner_address){throw 'context'}
 $pgnActions=@('read_access','create_role','edit_role','delete_role','move_role','assign_role','remove_role','set_overwrite','remove_content','pin_content')
 if($pgnCommand.action -cnotin $pgnActions -or $pgnCommand.args -isnot [System.Management.Automation.PSCustomObject]){throw 'action'}
 $pgnNow=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
 if($pgnCommand.created_ms -lt ($pgnNow-120000) -or $pgnCommand.created_ms -gt ($pgnNow+60000) -or $pgnCommand.revision -lt 0 -or $pgnCommand.request_id -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$'){throw 'freshness'}
 if($pgnRequest.challenge.Length -gt 8000 -or $pgnRequest.wallet.Length -gt 256){throw 'bound'}
 $pgnCookie=[System.IO.File]::ReadAllText([string]$pgnRequest.cookie).Trim()
 if($pgnCookie.Length -gt 1024 -or -not $pgnCookie.Contains(':')){throw 'cookie'}
 $pgnAuth=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pgnCookie))
 function Invoke-PgnAccessRpc([string]$pgnMethod,[object[]]$pgnArguments,[bool]$pgnWallet=$false){
  $pgnUrl='http://127.0.0.1:'+([int]$pgnRequest.rpc_port)+'/'
  if($pgnWallet){$pgnUrl+='wallet/'+[Uri]::EscapeDataString([string]$pgnRequest.wallet)}
  $pgnBody=@{jsonrpc='2.0';id='horizon-access-consent';method=$pgnMethod;params=@($pgnArguments)}|ConvertTo-Json -Depth 8 -Compress
  $pgnReply=Invoke-RestMethod -Method Post -Uri $pgnUrl -Headers @{Authorization=('Basic '+$pgnAuth)} -ContentType 'application/json' -Body ([Text.Encoding]::UTF8.GetBytes($pgnBody)) -TimeoutSec 10
  if($null -ne $pgnReply.error){throw 'rpc'}
  return $pgnReply.result
 }
 $pgnGenesis=Invoke-PgnAccessRpc 'getblockhash' @([int]0)
 if($pgnGenesis -cne $pgnRequest.genesis){$pgnReason='The mining wallet belongs to another network.';throw 'genesis'}
 $pgnOwner=Invoke-PgnAccessRpc 'getaddressinfo' @([string]$pgnRequest.miner_address) $true
 if($pgnOwner.ismine -ne $true -or $pgnOwner.scriptPubKey -cne $pgnRequest.script){$pgnReason='The selected mining wallet does not own this community account.';throw 'owner'}
 $pgnReason='The mining wallet could not sign the access confirmation. Use its normal wallet unlock controls if it is locked.'
 $pgnSignature=Invoke-PgnAccessRpc 'signmessage' @([string]$pgnRequest.miner_address,[string]$pgnRequest.challenge) $true
 if($pgnSignature -isnot [string] -or $pgnSignature.Length -lt 1 -or $pgnSignature.Length -gt 512){throw 'signature'}
 [Console]::Out.WriteLine((@{schema='PGN_REMOTE_ACCESS_SIGNED_1';genesis=$pgnRequest.genesis;script=$pgnRequest.script;signature=$pgnSignature}|ConvertTo-Json -Compress))
 exit 0
}catch{
 [Console]::Out.WriteLine((@{schema='PGN_REMOTE_ACCESS_SIGNED_1';error=$pgnReason}|ConvertTo-Json -Compress))
 exit 1
}
)PS");}
inline RemoteAccessConsentPlan remoteAccessConsentPlan(const QByteArray& profileBytes,
 const social::Identity& identity,const QString& community,const QString& script,const QByteArray& certificate){
 RemoteAccessConsentPlan p;
 const auto refuse=[&](QString reason){p.error=std::move(reason);return p;};
 const auto invalid=social::accessSigningError(certificate,identity,script,community);
 if(!invalid.isEmpty())return refuse(invalid);
 const auto command=QJsonDocument::fromJson(certificate.mid(certificate.indexOf('\n')+1)).object();
 if(profileBytes.isEmpty()||profileBytes.size()>65536)return refuse("The configured mining connection is unavailable.");
 QJsonParseError parse;const auto document=QJsonDocument::fromJson(profileBytes,&parse);const auto profile=document.object();
 if(parse.error!=QJsonParseError::NoError||!document.isObject())return refuse("The configured mining connection is unreadable.");
 const auto hardware=profile["hardware_endpoint"].toObject();const auto host=hardware["host"].toString(),user=hardware["user"].toString(),key=hardware["key_file"].toString(),alias=hardware["host_key_alias"].toString();
 const auto port=hardware["port"].toInt(22);p.program=profile["program"].toString();
 if(!QRegularExpression("^[A-Za-z0-9_.:-]{1,253}$").match(host).hasMatch()||!QRegularExpression("^[A-Za-z0-9_.-]{1,128}$").match(user).hasMatch()||!QRegularExpression("^[A-Za-z0-9_.-]{1,253}$").match(alias).hasMatch()||port<1||port>65535||!QFileInfo(key).isAbsolute()||!QFileInfo(key).isFile()||!QFileInfo(p.program).isAbsolute()||!QFileInfo(p.program).isFile()||!QStringList{"ssh","ssh.exe"}.contains(QFileInfo(p.program).fileName(),Qt::CaseInsensitive))
  return refuse("A verified mining connection is required to confirm these permissions.");
 const auto values=profile["network_arguments"].toArray();QStringList args;
 if(values.isEmpty()||values.size()>128)return refuse("The configured mining wallet is unavailable.");
 for(const auto& v:values){if(!v.isString()||v.toString().size()>4096)return refuse("The configured mining connection is invalid.");args.append(v.toString());}
 const auto option=[&](const QString& name){const auto at=args.indexOf(name);return at>=0&&at+1<args.size()&&args.count(name)==1?args[at+1]:QString{};};
 const auto network=args.indexOf("--network");
 if(network<1||!args[network-1].endsWith(".exe",Qt::CaseInsensitive))return refuse("Automatic access confirmation is not available for this mining connection yet.");
 if(option("--genesis")!=identity.genesis)return refuse("The configured mining wallet belongs to another network.");
 const auto wallet=option("--wallet"),cookie=option("--cookie"),miner=option("--miner");bool rpcOk{};const auto rpcPort=option("--rpc-port").toInt(&rpcOk);
 if(!rpcOk||rpcPort<1||rpcPort>65535||wallet.isEmpty()||wallet.size()>256||cookie.size()>1024||!QRegularExpression("^[A-Za-z]:[/\\\\]").match(cookie).hasMatch()||!QRegularExpression("^[A-Za-z0-9]{14,120}$").match(miner).hasMatch()||profile["miner_address"].toString()!=miner)
  return refuse("The configured mining wallet does not match this access confirmation.");
 if(command["address"]!=miner)return refuse("The access confirmation does not match the configured mining wallet.");
 for(const auto& s:{wallet,cookie})for(const auto ch:s)if(ch.unicode()<32||ch.unicode()==127)return refuse("The mining wallet configuration contains an invalid character.");
 const auto source=QString::fromUtf8(nativeAccessSignerPowerShell());QByteArray utf16;
 for(const auto ch:source){utf16.append(char(ch.unicode()&255));utf16.append(char(ch.unicode()>>8));}
 p.arguments={"-T","-i",key,"-p",QString::number(port),"-o","BatchMode=yes","-o","StrictHostKeyChecking=yes","-o","HostKeyAlias="+alias,"-o","ConnectTimeout=8","-o","ServerAliveInterval=5","-o","ServerAliveCountMax=2",user+"@"+host,"powershell.exe","-NoProfile","-NonInteractive","-EncodedCommand",QString::fromLatin1(utf16.toBase64())};
 const QJsonObject request{{"genesis",identity.genesis},{"script",script},{"client",identity.subject},{"community",community},{"rpc_port",rpcPort},{"cookie",cookie},{"wallet",wallet},{"miner_address",miner},{"challenge",QString::fromUtf8(certificate)}};
 p.input=QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n';
 if(p.input.size()>16000)return refuse("The access confirmation exceeds its supported size.");
 p.profileHash=QCryptographicHash::hash(profileBytes,QCryptographicHash::Sha256);return p;
}
inline void requestRemoteAccessConsent(QObject* owner,const QString& profilePath,const social::Identity& identity,
 const QString& community,const QString& script,const QByteArray& certificate,std::function<bool()> current,
 std::function<void(QString,QString)> done){
 if(!owner||!current||!current()){done({},"The wallet connection changed; no name was published.");return;}
 QFile file(profilePath);
 if(!file.open(QIODevice::ReadOnly)||file.size()>65536){done({},"The mining wallet connection is unavailable.");return;}
 const auto plan=remoteAccessConsentPlan(file.readAll(),identity,community,script,certificate);file.close();
 if(!plan.error.isEmpty()){done({},plan.error);return;}
 auto* process=new QProcess(owner);process->setProgram(plan.program);process->setArguments(plan.arguments);process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
 process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a){a->flags|=0x08000000;});
#endif
 const QPointer<QObject> lifetime(owner);const auto completed=std::make_shared<bool>(false);const auto output=std::make_shared<QByteArray>();
 const auto finish=[process,lifetime,completed,current,done](QString signature,QString error){if(*completed)return;*completed=true;if(lifetime){if(!current())done({},"The wallet connection changed; the access signature was discarded.");else done(std::move(signature),std::move(error));}process->deleteLater();};
 QObject::connect(process,&QProcess::started,process,[process,plan]{process->write(plan.input);process->closeWriteChannel();});
 QObject::connect(process,&QProcess::readyReadStandardOutput,process,[process,output,finish]{output->append(process->readAllStandardOutput());if(output->size()>8192){process->kill();finish({},"The mining wallet response exceeded its limit.");}});
 QObject::connect(process,&QProcess::readyReadStandardError,process,[process]{process->readAllStandardError();});
 QObject::connect(process,&QProcess::errorOccurred,process,[process,finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish({},"The mining wallet connection could not be started.");});
 QObject::connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),process,[process,output,finish,plan,profilePath,identity,script](int code,QProcess::ExitStatus status){
  output->append(process->readAllStandardOutput());QFile saved(profilePath);
  if(!saved.open(QIODevice::ReadOnly)||saved.size()>65536||QCryptographicHash::hash(saved.readAll(),QCryptographicHash::Sha256)!=plan.profileHash){finish({},"The selected mining connection changed; the access signature was discarded.");return;}
  if(output->size()>8192){finish({},"The mining wallet response exceeded its limit.");return;}
  QJsonParseError parse;const auto reply=QJsonDocument::fromJson(output->trimmed(),&parse).object();const auto signature=reply["signature"].toString();
  if(parse.error!=QJsonParseError::NoError||reply["schema"]!="PGN_REMOTE_ACCESS_SIGNED_1"||status!=QProcess::NormalExit||code!=0||reply["genesis"]!=identity.genesis||reply["script"]!=script||signature.isEmpty()||signature.size()>512){finish({},reply["error"].toString().left(300).isEmpty()?QString("The mining wallet could not confirm this community account."):reply["error"].toString().left(300));return;}
  // The access service verifies this proof against the exact immutable
  // command and current Native account before authorizing any mutation.
  finish(signature,{});
 });
 QTimer::singleShot(25000,process,[process,completed,finish]{if(!*completed){process->kill();finish({},"The mining wallet did not respond. Chat and mining remain available.");}});
 process->start();
}
} // namespace pgn::ui
