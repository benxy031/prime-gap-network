#include "wallet/core_rpc_diagnostic_history.h"
#include <QJsonDocument>
#include <iostream>
#include <string_view>

int main(int argc,char** argv){
    pgn::wallet::CoreRpcDiagnosticHistory history;
    auto append=[&](int sequence,const char* channel,const char* outcome){
        history.append({{"sequence",sequence},{"channel",channel},{"outcome",outcome}});
    };
    append(1,"inspection","credentials_unavailable");
    append(2,"ordinary","total_timeout");
    append(3,"inspection","credentials_unavailable");
    append(4,"inspection","total_timeout");
    append(5,"inspection","response_limit");
    for(int i=6;i<=124;++i)append(i,"ordinary","ok");
    const auto rows=history.snapshot();
    auto number=[&](qsizetype index){return rows[index].toObject().value("sequence").toInt();};
    if(rows.size()!=48||number(0)!=1||number(1)!=4||number(2)!=5){
        std::cerr<<"Late export lost initial inspection diagnostics or exceeded its limit\n";
        return 1;
    }
    for(qsizetype i=3;i<rows.size();++i)if(number(i)!=77+i){
        std::cerr<<"Late export no longer retains the recent RPC history in order\n";
        return 2;
    }
    pgn::wallet::CoreRpcDiagnosticHistory healthy;
    for(int i=1;i<=96;++i)healthy.append({{"sequence",i},{"channel","inspection"},{"outcome","ok"}});
    if(healthy.snapshot().size()!=48||healthy.snapshot().first().toObject().value("sequence").toInt()!=49)
        return 3;
    if(argc==2&&std::string_view(argv[1])=="--snapshot"){
        std::cout<<QJsonDocument(rows).toJson(QJsonDocument::Compact).constData();
    }else{
    std::cout<<"PASS first three unique inspection errors and latest 45 samples retained in order\n";
    }
}
