#pragma once
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QVector>
#include <algorithm>
#include <filesystem>

namespace pgn::ui {
using DiagnosticStorageRoots=QMap<QString,QString>;
struct DiagnosticStorageFolder {
 QString path;
 QString status;
 quint64 bytes{};
 QVector<QString> folders;
 bool partial{};
};
struct DiagnosticStorageInventory {
 DiagnosticStorageRoots roots;
 QHash<QString,DiagnosticStorageFolder> folders;
 quint64 uniqueRootBytes{};
 bool partial{};
 QStringList notes;
};
inline QString storagePathKey(QString path){
 path=QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
 return path.toCaseFolded();
#else
 return path;
#endif
}
inline std::filesystem::path storageNativePath(const QString& path){
#ifdef Q_OS_WIN
 return std::filesystem::path(path.toStdWString());
#else
 return std::filesystem::path(path.toStdString());
#endif
}
inline QString storageQtPath(const std::filesystem::path& path){
#ifdef Q_OS_WIN
 return QString::fromStdWString(path.native());
#else
 return QString::fromStdString(path.native());
#endif
}
inline DiagnosticStorageFolder scanStorageFolder(const QString& path,DiagnosticStorageInventory& inventory){
 const auto key=storagePathKey(path);
 if(const auto known=inventory.folders.constFind(key);known!=inventory.folders.cend())return known.value();
 DiagnosticStorageFolder folder{path};
 const QFileInfo located(path);
 if(located.isSymLink()||located.isJunction()){
  folder.status="Link · target not counted here";
  folder.partial=true;
 }else{
  std::error_code error;
  auto entries=std::filesystem::directory_iterator(storageNativePath(path),error);
  if(error){
   folder.status=located.exists()?"Cannot read this folder":"Folder unavailable";
   folder.partial=true;
   inventory.folders.insert(key,folder);
   return folder;
  }
  const std::filesystem::directory_iterator end;
  while(entries!=end&&!error){
   const auto entry=*entries;
   const auto entryPath=QDir::fromNativeSeparators(storageQtPath(entry.path()));
   const QFileInfo details(entryPath);
   if(details.isSymLink()||details.isJunction()||entry.is_directory(error)){
    const auto child=scanStorageFolder(entryPath,inventory);
    folder.folders.append(storagePathKey(entryPath));
    folder.partial=folder.partial||child.partial;
    folder.bytes+=child.bytes;
   }else if(!error&&entry.is_regular_file(error)){
    const auto size=entry.file_size(error);
    if(!error)folder.bytes+=size;
   }
   if(error){
    folder.partial=true;
    if(folder.status.isEmpty())folder.status="Some entries could not be read";
    error.clear();
   }
   entries.increment(error);
  }
  if(error){folder.status="Folder could only be read in part";folder.partial=true;}
  std::sort(folder.folders.begin(),folder.folders.end());
 }
 inventory.folders.insert(key,folder);
 return folder;
}
inline DiagnosticStorageInventory scanDiagnosticStorage(const DiagnosticStorageRoots& paths){
 DiagnosticStorageInventory result;
 QStringList unique;
 for(auto it=paths.cbegin();it!=paths.cend();++it){
  const QFileInfo declared(it.value());
  if(it.value().isEmpty())continue;
  if(!declared.isAbsolute()){
   result.roots.insert(it.key(),it.value());
   result.folders.insert(storagePathKey(it.value()),DiagnosticStorageFolder{it.value(),"Configured path is not absolute",0,{},true});
   result.partial=true;
   continue;
  }
  const auto resolved=declared.canonicalFilePath();
  const auto path=QDir::fromNativeSeparators(resolved.isEmpty()?declared.absoluteFilePath():resolved);
  QDir directory(path);
  if(directory.isRoot()){
   result.roots.insert(it.key(),path);
   result.folders.insert(storagePathKey(path),DiagnosticStorageFolder{path,"Whole drive not scanned",0,{},true});
   result.partial=true;
   continue;
  }
  result.roots.insert(it.key(),path);
  if(!unique.contains(path))unique.append(path);
 }
 std::sort(unique.begin(),unique.end(),[](const auto& first,const auto& second){return first.size()<second.size();});
 for(const auto& path:unique){
  const bool alreadyMeasured=result.folders.contains(storagePathKey(path));
  const auto folder=scanStorageFolder(path,result);
  if(!alreadyMeasured)result.uniqueRootBytes+=folder.bytes;
  result.partial=result.partial||folder.partial;
 }
 return result;
}
}
