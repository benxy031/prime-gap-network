#pragma once
#include "social/social_link_flow.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <memory>

namespace pgn::ui {
// Only the selected mining wallet may sign its own display-consent challenge.
// This reuses the configured SSH identity, keeps its RPC cookie on that device,
// and neither starts mining nor exposes a generic remote RPC/signing interface.
struct RemoteNameConsentPlan {
 QString program;QStringList arguments;QByteArray input,profileHash;QString error;
};
inline QByteArray nativeNameSignerPowerShell(){return QByteArray(R"PS(
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
 if(-not ($pgnRequest.challenge.StartsWith("PGN/SOCIAL/IDENTITY-LINK/OPERATOR-CONSENT/2`n") -or $pgnRequest.challenge.StartsWith("PGN/SOCIAL/IDENTITY-LINK/STATE-TRANSITION/2`n"))){throw 'purpose'}
 if($pgnRequest.challenge.Length -gt 8000 -or $pgnRequest.wallet.Length -gt 256){throw 'bound'}
 $pgnCookie=[System.IO.File]::ReadAllText([string]$pgnRequest.cookie).Trim()
 if($pgnCookie.Length -gt 1024 -or -not $pgnCookie.Contains(':')){throw 'cookie'}
 $pgnAuth=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pgnCookie))
 function Invoke-PgnNameRpc([string]$pgnMethod,[object[]]$pgnArguments,[bool]$pgnWallet=$false){
  $pgnUrl='http://127.0.0.1:'+([int]$pgnRequest.rpc_port)+'/'
  if($pgnWallet){$pgnUrl+='wallet/'+[Uri]::EscapeDataString([string]$pgnRequest.wallet)}
  $pgnBody=@{jsonrpc='2.0';id='horizon-display-consent';method=$pgnMethod;params=@($pgnArguments)}|ConvertTo-Json -Depth 8 -Compress
  $pgnReply=Invoke-RestMethod -Method Post -Uri $pgnUrl -Headers @{Authorization=('Basic '+$pgnAuth)} -ContentType 'application/json' -Body ([Text.Encoding]::UTF8.GetBytes($pgnBody)) -TimeoutSec 10
  if($null -ne $pgnReply.error){throw 'rpc'}
  return $pgnReply.result
 }
 $pgnGenesis=Invoke-PgnNameRpc 'getblockhash' @([int]0)
 if($pgnGenesis -cne $pgnRequest.genesis){$pgnReason='The mining wallet belongs to another network.';throw 'genesis'}
 $pgnOwner=Invoke-PgnNameRpc 'getaddressinfo' @([string]$pgnRequest.miner_address) $true
 if($pgnOwner.ismine -ne $true -or $pgnOwner.scriptPubKey -cne $pgnRequest.script){$pgnReason='The selected mining wallet does not own this public name.';throw 'owner'}
 $pgnReason='The mining wallet could not sign the name confirmation. Use its normal wallet unlock controls if it is locked.'
 $pgnSignature=Invoke-PgnNameRpc 'signmessage' @([string]$pgnRequest.miner_address,[string]$pgnRequest.challenge) $true
 if($pgnSignature -isnot [string] -or $pgnSignature.Length -lt 1 -or $pgnSignature.Length -gt 512){throw 'signature'}
 [Console]::Out.WriteLine((@{schema='PGN_REMOTE_DISPLAY_SIGNED_1';genesis=$pgnRequest.genesis;script=$pgnRequest.script;signature=$pgnSignature}|ConvertTo-Json -Compress))
 exit 0
}catch{
 [Console]::Out.WriteLine((@{schema='PGN_REMOTE_DISPLAY_SIGNED_1';error=$pgnReason}|ConvertTo-Json -Compress))
 exit 1
}
)PS");}
inline RemoteNameConsentPlan remoteNameConsentPlan(const QByteArray& profileBytes,
 const social::Identity& identity,social::NameLinkSide side,const QString& script,const QByteArray& certificate){
 RemoteNameConsentPlan p;
 const auto refuse=[&](QString reason){p.error=std::move(reason);return p;};
 social::NameLinkPacket packet;const auto invalid=social::decodeNameLinkPacket(certificate,identity.genesis,packet);
 if(!identity.valid()||!invalid.isEmpty()||packet.socialScript!=identity.subject||packet.nativeScript!=script)
  return refuse("The name confirmation no longer matches this chat identity.");
 const bool bind=side==social::NameLinkSide::Native&&packet.action=="bind";
 const bool withdraw=side==social::NameLinkSide::Transition&&packet.action=="withdraw"&&packet.certificate["authorizer_script"]==script;
 if(!bind&&!withdraw)return refuse("Only the mining wallet's explicit name confirmation can use this connection.");
 if(profileBytes.isEmpty()||profileBytes.size()>65536)return refuse("The configured mining connection is unavailable.");
 QJsonParseError parse;const auto document=QJsonDocument::fromJson(profileBytes,&parse);const auto profile=document.object();
 if(parse.error!=QJsonParseError::NoError||!document.isObject())return refuse("The configured mining connection is unreadable.");
 const auto hardware=profile["hardware_endpoint"].toObject();const auto host=hardware["host"].toString(),user=hardware["user"].toString(),key=hardware["key_file"].toString(),alias=hardware["host_key_alias"].toString();
 const auto port=hardware["port"].toInt(22);p.program=profile["program"].toString();
 if(!QRegularExpression("^[A-Za-z0-9_.:-]{1,253}$").match(host).hasMatch()||!QRegularExpression("^[A-Za-z0-9_.-]{1,128}$").match(user).hasMatch()||!QRegularExpression("^[A-Za-z0-9_.-]{1,253}$").match(alias).hasMatch()||port<1||port>65535||!QFileInfo(key).isAbsolute()||!QFileInfo(key).isFile()||!QFileInfo(p.program).isAbsolute()||!QFileInfo(p.program).isFile()||!QStringList{"ssh","ssh.exe"}.contains(QFileInfo(p.program).fileName(),Qt::CaseInsensitive))
  return refuse("A verified mining connection is required to confirm this name.");
 const auto values=profile["network_arguments"].toArray();QStringList args;
 if(values.isEmpty()||values.size()>128)return refuse("The configured mining wallet is unavailable.");
 for(const auto& v:values){if(!v.isString()||v.toString().size()>4096)return refuse("The configured mining connection is invalid.");args.append(v.toString());}
 const auto option=[&](const QString& name){const auto at=args.indexOf(name);return at>=0&&at+1<args.size()&&args.count(name)==1?args[at+1]:QString{};};
 const auto network=args.indexOf("--network");
 if(network<1||!args[network-1].endsWith(".exe",Qt::CaseInsensitive))return refuse("Automatic name confirmation is not available for this mining connection yet.");
 if(option("--genesis")!=identity.genesis)return refuse("The configured mining wallet belongs to another network.");
 const auto wallet=option("--wallet"),cookie=option("--cookie"),miner=option("--miner");bool rpcOk{};const auto rpcPort=option("--rpc-port").toInt(&rpcOk);
 if(!rpcOk||rpcPort<1||rpcPort>65535||wallet.isEmpty()||wallet.size()>256||cookie.size()>1024||!QRegularExpression("^[A-Za-z]:[/\\\\]").match(cookie).hasMatch()||!QRegularExpression("^[A-Za-z0-9]{14,120}$").match(miner).hasMatch()||profile["miner_address"].toString()!=miner)
  return refuse("The configured mining wallet does not match this name confirmation.");
 for(const auto& s:{wallet,cookie})for(const auto ch:s)if(ch.unicode()<32||ch.unicode()==127)return refuse("The mining wallet configuration contains an invalid character.");
 const auto source=QString::fromUtf8(nativeNameSignerPowerShell());QByteArray utf16;
 for(const auto ch:source){utf16.append(char(ch.unicode()&255));utf16.append(char(ch.unicode()>>8));}
 p.arguments={"-T","-i",key,"-p",QString::number(port),"-o","BatchMode=yes","-o","StrictHostKeyChecking=yes","-o","HostKeyAlias="+alias,"-o","ConnectTimeout=8","-o","ServerAliveInterval=5","-o","ServerAliveCountMax=2",user+"@"+host,"powershell.exe","-NoProfile","-NonInteractive","-EncodedCommand",QString::fromLatin1(utf16.toBase64())};
 const QJsonObject request{{"genesis",identity.genesis},{"script",script},{"rpc_port",rpcPort},{"cookie",cookie},{"wallet",wallet},{"miner_address",miner},{"challenge",QString::fromUtf8(bind?packet.nativeChallenge:packet.transitionChallenge)}};
 p.input=QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n';
 if(p.input.size()>16000)return refuse("The name confirmation exceeds its supported size.");
 p.profileHash=QCryptographicHash::hash(profileBytes,QCryptographicHash::Sha256);return p;
}
inline void requestRemoteNameConsent(QObject* owner,const QString& profilePath,const social::Identity& identity,
 social::NameLinkSide side,const QString& script,const QByteArray& certificate,std::function<bool()> current,
 std::function<void(QString,QString)> done){
 if(!owner||!current||!current()){done({},"The wallet connection changed; no name was published.");return;}
 QFile file(profilePath);
 if(!file.open(QIODevice::ReadOnly)||file.size()>65536){done({},"The mining wallet connection is unavailable.");return;}
 const auto plan=remoteNameConsentPlan(file.readAll(),identity,side,script,certificate);file.close();
 if(!plan.error.isEmpty()){done({},plan.error);return;}
 auto* process=new QProcess(owner);process->setProgram(plan.program);process->setArguments(plan.arguments);process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
 process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a){a->flags|=0x08000000;});
#endif
 const QPointer<QObject> lifetime(owner);const auto completed=std::make_shared<bool>(false);const auto output=std::make_shared<QByteArray>();
 const auto finish=[process,lifetime,completed,current,done](QString signature,QString error){if(*completed)return;*completed=true;if(lifetime){if(!current())done({},"The wallet connection changed; the name signature was discarded.");else done(std::move(signature),std::move(error));}process->deleteLater();};
 QObject::connect(process,&QProcess::started,process,[process,plan]{process->write(plan.input);process->closeWriteChannel();});
 QObject::connect(process,&QProcess::readyReadStandardOutput,process,[process,output,finish]{output->append(process->readAllStandardOutput());if(output->size()>8192){process->kill();finish({},"The mining wallet response exceeded its limit.");}});
 QObject::connect(process,&QProcess::readyReadStandardError,process,[process]{process->readAllStandardError();});
 QObject::connect(process,&QProcess::errorOccurred,process,[process,finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish({},"The mining wallet connection could not be started.");});
 QObject::connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),process,[process,output,finish,plan,profilePath,identity,script](int code,QProcess::ExitStatus status){
  output->append(process->readAllStandardOutput());QFile saved(profilePath);
  if(!saved.open(QIODevice::ReadOnly)||saved.size()>65536||QCryptographicHash::hash(saved.readAll(),QCryptographicHash::Sha256)!=plan.profileHash){finish({},"The selected mining connection changed; the name signature was discarded.");return;}
  if(output->size()>8192){finish({},"The mining wallet response exceeded its limit.");return;}
  QJsonParseError parse;const auto reply=QJsonDocument::fromJson(output->trimmed(),&parse).object();const auto signature=reply["signature"].toString();
  if(parse.error!=QJsonParseError::NoError||reply["schema"]!="PGN_REMOTE_DISPLAY_SIGNED_1"||status!=QProcess::NormalExit||code!=0||reply["genesis"]!=identity.genesis||reply["script"]!=script||signature.isEmpty()||signature.size()>512){finish({},reply["error"].toString().left(300).isEmpty()?QString("The mining wallet could not confirm this public name."):reply["error"].toString().left(300));return;}
  // The normal display verifier subsequently checks this BIP322 proof against
  // the exact packet and current Native identity before any publication.
  finish(signature,{});
 });
 QTimer::singleShot(25000,process,[process,completed,finish]{if(!*completed){process->kill();finish({},"The mining wallet did not respond. Chat and mining remain available.");}});
 process->start();
}
} // namespace pgn::ui
