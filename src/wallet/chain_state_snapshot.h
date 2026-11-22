#pragma once
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>
#include <atomic>
#include <memory>
#include <zlib.h>

namespace pgn::wallet {
struct BootstrapStateMember {
 const char* path;
 qsizetype start,compressed,bytes;
 const char* sha256;
};
inline constexpr char BootstrapStateAsset[]="https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/PGN-Testnet-State-22489.zip";
inline constexpr char BootstrapStateSha256[]="b42e84f1616b134cf171e4ddbac4fe0468f9d4ae56a31fd2a4b6b0c025ddf052";
inline constexpr qsizetype BootstrapStateBytes=60205748;
inline constexpr qint64 BootstrapStateHeight=22489;
// Each member is pinned to the exact testnet release asset.
inline constexpr BootstrapStateMember BootstrapStateMembers[]{
 {"blocks/blk00000.dat",49,16211378,50331648,"cb45a525dda410bc8e246bcfb3e6949e3d1a7d7f2f21532dfe10852d2c1fbb7a"},
 {"blocks/index/000160.ldb",16211480,2779811,3968106,"e90e2c5a4f90622ace3ceb92d7fea3417d38aef0c331dc38d68f0c6d6fdb0969"},
 {"blocks/index/000162.ldb",18991344,5204,9943,"7c8aaeb123ffe0ff7f1885ce6f4914aeeb075819f107aa53b7c6f403a6784bec"},
 {"blocks/index/000165.ldb",18996601,2225,4068,"bec4a508e32d98cb8ae5a72ee821028602ff1a258e28c6c34f926a99aa54ff0b"},
 {"blocks/index/000166.log",18998879,2692,5067,"15cabc5fb3c92056bbade89526ec7106ee9e295120f905e9f95caea4e840c9b2"},
 {"blocks/index/CURRENT",19001621,18,16,"0455c8ef615ed6d87c651fc2e1ebb5c5e7d49d00d2a77ec17f3cb0983eeb79af"},
 {"blocks/index/MANIFEST-000164",19001697,219,242,"5ee4de41fa96dfc9830b7875a90f197c8f5a1fd4c8d4069104c1eb2d21190f80"},
 {"blocks/rev00000.dat",19001965,780461,1048576,"acecb43b2ab0fddefb949589b37114f6978d58b8d81c85e72c969d2c0b007dc6"},
 {"blocks/xor.dat",19782470,10,8,"75991ec4fdb1df26b03e72fc015c038d0c5fb6510e61f761fcff93a4cefc3fbd"},
 {"chainstate-pgn-request-history/023160.ldb",19782551,11380027,33970412,"9a804251b1108ca3064e2f5e5f095040e00a9f29de3c899d2659c33cce1995ee"},
 {"chainstate-pgn-request-history/027346.log",31162649,5356,15466,"10d013106f2904d052f9f0d48cac74a059cc35d05f9cd29c57c6009561cf0a41"},
 {"chainstate-pgn-request-history/027349.ldb",31168076,9496282,33809015,"dedc56c9ea3ae5719a915afa844fedca516e61071a55b194b3cf2bdf5a95a3b9"},
 {"chainstate-pgn-request-history/027350.ldb",40664429,9626554,33791279,"2c446f3e36a833830d7124fbac2f72d595ffbeb9475b5e7d685d3791bab29142"},
 {"chainstate-pgn-request-history/027351.ldb",50291054,2865444,10475524,"4eb3605097e43380a1ff4d5b3dae4724d3383f80c1c90cba0340cb5d4b426828"},
 {"chainstate-pgn-request-history/CURRENT",53156566,18,16,"063b4c1bd04b2516661f2699af8e5b8f3f854d5a1b91c0f625fd6cc9ff690bd3"},
 {"chainstate-pgn-request-history/MANIFEST-027260",53156660,3686,14162,"9720146a9d8587cec0ece78936bff7fb7e6d1225b8547fdd392b26fd2c6bba75"},
 {"chainstate/000120.ldb",53160397,5637714,11609235,"be7176f63641bb1e7edba7c2af8f1d03814dda89cd3f28de9969e7b510959749"},
 {"chainstate/000161.ldb",58798162,1359835,3138709,"65d206b922588ea796a302fba21b2959ced9896fa6df5944f26810165c21ea1b"},
 {"chainstate/000163.ldb",60158048,9683,38694,"a89fad11fd016eaf3338ee720657f8c8725da80e70ede6b26725de7a604f0fa3"},
 {"chainstate/000166.ldb",60167782,18341,67342,"eca717e7d9fcc0b302720d3e498f051a25a90aa4db89f0fdf882a3377cf018bf"},
 {"chainstate/000169.ldb",60186174,7168,27973,"d5415795287d0df4189868826b1b2b59d592fc9ad4d5a484710f0041ca5d78f7"},
 {"chainstate/000170.log",60193393,8694,37023,"d6dc5e1893e3bae3511efaebbc81f49673b1742db7870850775c935db608150a"},
 {"chainstate/CURRENT",60202135,18,16,"c8f01f055956223c14e593780c7fb8f9b7caa80a9600579b319c70783a2f38ed"},
 {"chainstate/MANIFEST-000168",60202209,173,228,"d967ec946cda668b525352d905c5010daa442bd0db9a45142e6309201721ab1a"},
};
inline bool freshBootstrapStateTarget(const QString& dataDirectory){
 const QFileInfo root(dataDirectory),network(QDir(dataDirectory).filePath("testnet"));
 return root.isAbsolute()&&root.isDir()&&!root.isSymLink()&&!root.isJunction()
     &&!network.exists()&&!network.isSymLink()&&!network.isJunction();
}
inline bool extractBootstrapStateMember(const QByteArray& bundle,const BootstrapStateMember& item,
       const QString& destination,const std::atomic_bool& cancelled){
 if(item.start<0||item.bytes<0||item.compressed<=0||item.start>bundle.size()-item.compressed)return false;
 if(!QDir().mkpath(QFileInfo(destination).absolutePath()))return false;
 QSaveFile file(destination);
 if(!file.open(QIODevice::WriteOnly))return false;
 QCryptographicHash content(QCryptographicHash::Sha256);
 z_stream stream{};
 stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(bundle.constData()+item.start));
 stream.avail_in=uInt(item.compressed);
 if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK)return false;
 char buffer[64*1024];
 qsizetype written=0;
 int status=Z_OK;
 while(status==Z_OK&&!cancelled.load()){
  stream.next_out=reinterpret_cast<Bytef*>(buffer);
  stream.avail_out=sizeof(buffer);
  status=inflate(&stream,Z_NO_FLUSH);
  const auto count=qsizetype(sizeof(buffer)-stream.avail_out);
  if(written+count>item.bytes||file.write(buffer,count)!=count){status=Z_DATA_ERROR;break;}
  content.addData(QByteArrayView(buffer,count));
  written+=count;
 }
 const bool verified=status==Z_STREAM_END&&stream.avail_in==0&&written==item.bytes
     &&content.result().toHex()==item.sha256&&!cancelled.load();
 inflateEnd(&stream);
 return verified&&file.commit();
}
class StagedBootstrapState {
public:
 StagedBootstrapState(QString root, QString stage):root_(std::move(root)),stage_(std::move(stage)){}
 ~StagedBootstrapState(){
  const QFileInfo owned(QDir(root_).filePath(stage_));
  const auto canonical=owned.canonicalFilePath();
  if(!canonical.isEmpty()&&!owned.isSymLink()&&!owned.isJunction()
      &&QFileInfo(canonical).absolutePath()==root_&&QFileInfo(canonical).fileName()==stage_)
   QDir(canonical).removeRecursively();
 }
 StagedBootstrapState(const StagedBootstrapState&)=delete;
 StagedBootstrapState& operator=(const StagedBootstrapState&)=delete;

 QString contents() const {return QDir(root_).filePath(stage_+"/chain");}
 bool publish(const QString& dataDirectory) const {
  return freshBootstrapStateTarget(dataDirectory)&&QFileInfo(dataDirectory).canonicalFilePath()==root_
      &&QDir().rename(contents(),QDir(root_).filePath("testnet"));
 }

private:
 QString root_,stage_;
};
inline std::shared_ptr<StagedBootstrapState> stageVerifiedBootstrapState(
    const QByteArray& bundle,const QString& dataDirectory,const std::atomic_bool& cancelled){
 if(!freshBootstrapStateTarget(dataDirectory)||cancelled.load()||bundle.size()!=BootstrapStateBytes
    ||QCryptographicHash::hash(bundle,QCryptographicHash::Sha256).toHex()!=BootstrapStateSha256)return {};
 const QDir root(QFileInfo(dataDirectory).canonicalFilePath());
 const auto stageName=QStringLiteral(".pgn-state-")+QUuid::createUuid().toString(QUuid::Id128);
 if(cancelled.load()||!root.mkdir(stageName))return {};
 auto stage=std::make_shared<StagedBootstrapState>(root.absolutePath(),stageName);
 const QDir target(stage->contents());
 for(const auto& item:BootstrapStateMembers){
  if(!extractBootstrapStateMember(bundle,item,target.filePath(QString::fromLatin1(item.path)),cancelled))return {};
 }
 return cancelled.load()?nullptr:std::move(stage);
}
}
