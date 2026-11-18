#include "wallet/core_rpc_diagnostic_history.h"
#include <QJsonDocument>
#include <QNetworkReply>
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
    pgn::wallet::CoreRpcDiagnosticHistory link;
    auto transport=[&](int sequence,const char* method,const char* outcome,int transportCode){
        link.append({{"sequence",sequence},{"channel","inspection"},{"method",method},
                     {"outcome",outcome},{"transport_code",transportCode}});
    };
    transport(1,"getconnectioncount","transport_error",QNetworkReply::TimeoutError);
    transport(2,"getblockchaininfo","transport_error",QNetworkReply::ConnectionRefusedError);
    transport(3,"getblockchaininfo","transport_error",QNetworkReply::TimeoutError);
    transport(4,"getblockhash","total_timeout",0);
    transport(5,"getblockhash","transport_error",QNetworkReply::TimeoutError);
    transport(6,"getblockchaininfo","total_timeout",0);
    for(int i=7;i<=124;++i)transport(i,"getblockchaininfo","ok",0);
    const auto linkRows=link.snapshot();
    if(linkRows.size()!=48||linkRows[0].toObject().value("sequence")!=1
        ||linkRows[1].toObject().value("sequence")!=3
        ||linkRows[2].toObject().value("sequence")!=4
        ||linkRows[3].toObject().value("sequence")!=80){
        std::cerr<<"An earlier auxiliary or non-timeout error masked a chain inspection timeout\n";
        return 4;
    }
    if(argc==2&&std::string_view(argv[1])=="--snapshot"){
        std::cout<<QJsonDocument(rows).toJson(QJsonDocument::Compact).constData();
    }else{
    std::cout<<"PASS first three unique inspection errors and latest 45 samples retained in order\n";
    }
}
