#include "ui/network_work_view.h"
#include "ui/panel_style.h"
#include "ui/render/horizon_workspace_style.h"
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <iostream>
#include <stdexcept>
#include <QTreeWidgetItemIterator>
using pgn::ui::NetworkWorkView;
int main(int argc,char** argv){QApplication app(argc,argv);app.setStyleSheet(pgn::ui::dataPanelStyle());int checks=0;const auto need=[&](bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);};try{
 QJsonArray leases,reports,audits;for(int i=1;i<=100;++i){const auto id=QString::number(i,16).rightJustified(64,'0');leases.append(QJsonObject{{"request",id},{"ticket_number",QString::number(i)},{"start","150000000000000000000"},{"end","150000200000000000000"},{"state","reported"},{"issued",i},{"report_root",QString(64,'c')}});reports.append(QJsonObject{{"request_id",id},{"report_root",QString(64,'c')},{"block",QString(64,'d')},{"height",120}});audits.append(QJsonObject{{"request_id",id},{"jobs",QJsonArray{QJsonObject{{"slot",0},{"attempt",0},{"state","assigned"}}}}});}
 QJsonObject page{{"leases",leases},{"reports",reports},{"audits",audits},{"genesis",QString(64,'a')},{"bestblockhash",QString(64,'b')},{"height",150},{"active_lease_count",100}};int pageReads=0,detailReads=0;QString error;
 NetworkWorkView view([&](int,NetworkWorkView::Result consume){++pageReads;consume(page,error);},[&](const QString&,NetworkWorkView::Result consume){++detailReads;consume({},"Unexpected per-ticket RPC");});view.resize(1280,800);view.show();app.processEvents();
 auto* table=view.findChild<QTableWidget*>("pgn-network-ticket-table");auto* tree=view.findChild<QTreeWidget*>();QPushButton* refresh=nullptr;for(auto* b:view.findChildren<QPushButton*>())if(b->text()=="Refresh work")refresh=b;need(table&&tree&&refresh,"real work controls");need(table->rowCount()==100,"100 real widget rows");
 for(int i=0;i<100;++i){table->setCurrentCell(i,0);table->selectRow(i);app.processEvents();const auto selected=view.property("pgn_selected_ticket_evidence").toJsonObject();need(selected.value("leases").toArray().size()==1,"page projection owns exactly one lease");need(selected.value("reports").toArray().size()==1&&selected.value("audits").toArray().size()==1,"matching report/audit projected from same page");need(selected.value("leases").toArray().first().toObject().value("request")==leases[i].toObject().value("request"),"selected identity exact");need(selected.value("bestblockhash")==page.value("bestblockhash"),"one coherent tip");}
 need(detailReads==0,"all100 page selections immediate without extra detail RPC");
 auto named=audits[99].toObject();named.insert("public_attribution",QJsonObject{{"schema","PGN_PUBLIC_ATTRIBUTION_V1"},{"name","Alice"}});audits[99]=named;page.insert("audits",audits);page.insert("bestblockhash",QString(64,'9'));refresh->click();app.processEvents();bool sawName=false,sawOwner=false;for(int i=0;i<tree->topLevelItemCount();++i){const auto* item=tree->topLevelItem(i);if(item->text(0)=="Public discoverer name")sawName=item->text(1)=="Alice";if(item->text(0)=="Owner signing script")sawOwner=true;}need(sawName&&sawOwner,"public name shown beside retained signing identity");
 auto* cell=table->item(99,0);auto* evidence=tree->topLevelItem(0);const auto rebuilds=view.property("pgn_ticket_detail_rebuilds").toInt();page.insert("height",151);page.insert("bestblockhash",QString(64,'e'));refresh->click();app.processEvents();
 need(table->item(99,0)==cell&&tree->topLevelItem(0)==evidence,"anchor-only refresh keeps actual row and evidence objects");need(view.property("pgn_ticket_detail_rebuilds").toInt()==rebuilds,"anchor-only refresh does not rebuild detail tree");need(table->currentRow()==99,"selection preserved");need(view.property("pgn_selected_ticket_evidence").toJsonObject().value("height")==151,"anchor metadata still advances");
 for(int n=0;n<3;++n){page.insert("height",152+n);refresh->click();app.processEvents();need(tree->topLevelItem(0)==evidence&&table->item(99,0)==cell,"repeated clock-only refresh stable");}
 auto changedLease=leases[99].toObject();changedLease.insert("audited",true);leases[99]=changedLease;page.insert("leases",leases);auto changed=audits[99].toObject();changed.insert("audited",true);auto jobs=changed.value("jobs").toArray();auto job=jobs[0].toObject();job.insert("state","passed");jobs[0]=job;changed.insert("jobs",jobs);audits[99]=changed;page.insert("audits",audits);page.insert("bestblockhash",QString(64,'f'));refresh->click();app.processEvents();need(table->item(99,0)==cell,"real data delta updates existing row");need(table->item(99,4)->text()=="Assigned audits passed","stage genuinely updated");need(view.property("pgn_ticket_detail_rebuilds").toInt()==rebuilds+1,"real audit delta rebuilds details once");
 const auto tableGeometry=table->geometry();const auto treeGeometry=tree->geometry();
 error="Injected Core transport loss";evidence=tree->topLevelItem(0);refresh->click();app.processEvents();need(table->rowCount()==100&&tree->topLevelItem(0)==evidence,"transport failure retains verified content");need(detailReads==0,"no hidden per-row RPC fallback");need(table->geometry()==tableGeometry&&tree->geometry()==treeGeometry,"transport status never shifts table or evidence geometry");auto* status=view.findChild<QLabel*>("pgn-network-work-status");need(status&&!status->isVisible()&&status->toolTip()==error,"duplicate orange status stays hidden while complete failure remains diagnostic");error="A long Core transport failure "+QString(400,'x');refresh->click();app.processEvents();need(table->geometry()==tableGeometry&&tree->geometry()==treeGeometry,"long failure stays in the same fixed command row");
 QJsonObject thin=page;thin.insert("reports",QJsonArray{});thin.insert("audits",QJsonArray{});int thinReads=0;
 NetworkWorkView missing([&](int,NetworkWorkView::Result done){done(thin,{});},[](const QString&,NetworkWorkView::Result done){done({},"Unavailable");});
 missing.resize(1280,800);missing.show();app.processEvents();
 auto* missingTable=missing.findChild<QTableWidget*>("pgn-network-ticket-table");need(missingTable->item(0,4)->text()=="Status unavailable","omitted audit arrays must never become a false awaiting verdict");
 auto batch=thin;batch.insert("audits",audits);
 NetworkWorkView batched([&](int,NetworkWorkView::Result done){done(batch,{});},[](const QString&,NetworkWorkView::Result done){done({},"Detail omitted");});
 batched.resize(1280,800);batched.show();app.processEvents();
 auto* batchedTable=batched.findChild<QTableWidget*>("pgn-network-ticket-table");need(batchedTable->item(99,4)->text()=="Assigned audits passed","batched audit state overrides stale submitted lease state without report details");need(batchedTable->item(0,4)->text()=="Audits assigned","batched audit assignment and passed rows remain distinct");
 need(batchedTable->item(0,4)->foreground().color()==pgn::ui::ticketStagePresentation("audits_assigned").color,"table assignment color equals shared atlas stage color");need(batchedTable->item(0,4)->foreground().color()!=pgn::ui::ticketStagePresentation("assigned").color,"audit assignment is distinct from search-ticket assignment");
 need(pgn::ui::auditReportPresentation(QJsonObject{{"jobs",QJsonArray{QJsonObject{{"state","committed"}}}}}).color==pgn::ui::ticketStagePresentation("audits_assigned").color,"committed reports share their aggregate atlas lane color");
 const auto legend=batched.findChild<QLabel*>("pgn-network-state-legend");need(legend&&legend->text()==pgn::ui::ticketStageLegendText(),"table legend uses the same palette and stage descriptions");
 NetworkWorkView partial([&](int,NetworkWorkView::Result consume){consume(thin,{});},[&](const QString& id,NetworkWorkView::Result consume){++thinReads;auto value=page;QJsonArray one;for(const auto& row:leases)if(row.toObject().value("request").toString()==id)one.append(row);value.insert("leases",one);for(const auto* key:{"reports","audits"}){QJsonArray found;for(const auto& row:page.value(key).toArray())if(row.toObject().value("request_id").toString()==id)found.append(row);value.insert(key,found);}consume(value,{});});
 partial.resize(1280,800);partial.show();for(int n=0;n<30;++n)app.processEvents();need(thinReads>=2&&thinReads<=9,"partial page uses bounded visible prefetch, not100detailRPC");
 auto* partialTable=partial.findChild<QTableWidget*>("pgn-network-ticket-table");const int prefetched=thinReads;partialTable->setCurrentCell(1,0);partialTable->selectRow(1);app.processEvents();need(thinReads==prefetched,"prefetched visible selection is immediate without another RPC");const auto exact=partial.property("pgn_selected_ticket_evidence").toJsonObject();need(exact.value("reports").toArray().size()==1&&exact.value("audits").toArray().size()==1,"omitted page report/audit is fetched, never labeled absent");need(partial.property("pgn_ticket_detail_cache_size").toInt()<=64,"detail cache remains bounded");
 const QString first="39614081257132168796771975127",last="39614081257132168796771975167";
 auto preciseLease=leases[0].toObject();preciseLease.insert("start",first);preciseLease.insert("end",last);
 auto preciseAudit=audits[0].toObject();auto preciseJobs=preciseAudit["jobs"].toArray();auto preciseJob=preciseJobs[0].toObject();preciseJob.insert("ranges",QJsonArray{QJsonObject{{"index",0},{"start",first},{"end",last}}});preciseJobs[0]=preciseJob;preciseAudit.insert("jobs",preciseJobs);
 auto precisePage=page;precisePage.insert("leases",QJsonArray{preciseLease});precisePage.insert("reports",QJsonArray{reports[0]});precisePage.insert("audits",QJsonArray{preciseAudit});
 NetworkWorkView precise([&](int,NetworkWorkView::Result consume){consume(precisePage,{});},[&](const QString&,NetworkWorkView::Result){need(false,"coherent precise page needs no detail RPC");});precise.resize(1280,800);precise.show();app.processEvents();
 auto* preciseTree=precise.findChild<QTreeWidget*>();QString startText,endText;QTreeWidgetItem* partition=nullptr;
 for(QTreeWidgetItemIterator item(preciseTree);*item;++item){if((*item)->text(0)=="Start · scientific / binary")startText=(*item)->text(1);if((*item)->text(0)=="End · scientific / binary")endText=(*item)->text(1);if((*item)->text(0)=="Partition 0")partition=*item;}
 const auto coordinates=pgn::ui::positionRangeFormats(first,last);
 need(startText.contains(coordinates.start.decimal)&&endText.contains(coordinates.end.decimal)&&startText!=endText,"close95bit scientific bounds remain visibly distinct");
 need(partition&&partition->text(1)==pgn::ui::positionFormats("40").inlineText()+" positions","audit partition uses common three-notation quantity");need(partition->childCount()==3&&partition->child(0)->text(1)=="["+first+", "+last+")","audit exact bounds remain selectable below compact quantity");
 // Audit slots remain compact and selected evidence retains the exact quantity.
 const auto selectedBefore=view.property("pgn_selected_ticket_evidence");
 auto* detailBefore=tree->topLevelItem(0);const int readsBefore=pageReads,detailsBefore=detailReads;
 need(table->columnCount()==7&&table->horizontalHeaderItem(2)->text()=="Audit 1"&&table->horizontalHeaderItem(3)->text()=="Audit 2","distinct adjacent audit columns");
 for(int width:{640,900,1400,1920,640,1920}){
  view.resize(width,800);for(int n=0;n<6;++n)app.processEvents();
  need(table->rowHeight(99)==std::max(22,table->fontMetrics().lineSpacing()+6),"audit indicators retain compact rows");
  for(int col:{0,1,2,3,4,5,6})need(table->columnWidth(col)>=table->sizeHintForIndex(table->model()->index(99,col)).width(),"themed delegate fits complete cell");
  need(table->currentRow()==99&&table->selectionModel()->isRowSelected(99,QModelIndex()),"resize retains row selection");
  need(tree->topLevelItem(0)==detailBefore&&view.property("pgn_selected_ticket_evidence")==selectedBefore,"resize preserves exact evidence without rebuilding");
 }
 need(pageReads==readsBefore&&detailReads==detailsBefore,"resizing adds no network reads");
 const auto indicator=pgn::ui::auditSlotIndicator(QJsonObject{{"jobs",QJsonArray{QJsonObject{{"slot",0},{"state","passed"}},QJsonObject{{"slot",1},{"state","committed"}}}}},0,true);
 need(indicator.state=="passed","only an explicitly passed slot gets a success indicator");
 need(pgn::ui::auditSlotIndicator(QJsonObject{{"audited",true}},0,true).state=="unavailable","aggregate success without slot data never fabricates a check");
 need(pgn::ui::auditSlotIndicator(QJsonObject{{"jobs",QJsonArray{QJsonObject{{"slot",0},{"state","passed"}},QJsonObject{{"slot",0},{"state","passed"}}}}},0,true).state=="unavailable","duplicate slots do not fabricate success");
 need(table->item(99,2)->data(Qt::UserRole+2).toString()=="passed"&&!table->item(99,2)->icon().isNull(),"real widget exposes confirmed passed slot with native icon");
 need(table->item(99,3)->data(Qt::UserRole+2).toString()=="unavailable","missing second slot on a validated aggregate remains unavailable");
 need(table->item(99,2)->data(Qt::AccessibleTextRole).toString()=="Audit 1: Passed","icon retains accessible meaning");
 auto broadLease=preciseLease;broadLease.insert("start","0");auto broadPage=precisePage;broadPage.insert("leases",QJsonArray{broadLease});
 NetworkWorkView broad([&](int,NetworkWorkView::Result consume){consume(broadPage,{});},[&](const QString&,NetworkWorkView::Result){need(false,"wide95bit page needs no detail read");});broad.resize(640,800);broad.show();for(int n=0;n<6;++n)app.processEvents();
 auto* broadTable=broad.findChild<QTableWidget*>("pgn-network-ticket-table");const auto broadCount=pgn::ui::positionFormats(last);QString broadText;
 for(QTreeWidgetItemIterator item(broad.findChild<QTreeWidget*>());*item;++item)if((*item)->text(0)=="Positions")broadText=(*item)->text(1);
 need(broadText==broadCount.inlineText(),"exact95bit quantity remains in evidence");
 need(broadTable->item(0,1)->text()=="0 → "+last,"audit columns leave exact95bit interval unchanged");
 auto* mockAnimation=new QWidget;mockAnimation->setMinimumHeight(220);view.setAuditAnimation(mockAnimation);view.resize(1280,900);app.processEvents();auto* split=view.findChild<QSplitter*>("pgn-network-map-split");split->setSizes({450,200});app.processEvents();const int changedHeight=mockAnimation->height();const auto chosen=view.property("pgn_selected_ticket_evidence");const int beforeResetReads=pageReads;view.atlasCommand(QJsonObject{{"action","reset_layout"}});app.processEvents();need(changedHeight!=mockAnimation->height()&&mockAnimation->height()==290,"Reset view restores exact default animation height");need(view.property("pgn_selected_ticket_evidence")==chosen&&pageReads==beforeResetReads,"Reset view preserves selected evidence and does no RPC");

 {
 QJsonArray ordered;for(int i=300;i>0;--i){auto row=leases.first().toObject();row.insert("request",QString::number(i,16).rightJustified(64,'0'));row.insert("ticket_number",QString::number(i));row.insert("issued",i);ordered.append(row);}
 int seekReads=0,remoteDetails=0;
 const auto one=[&](const QString& id){for(const auto& v:ordered)if(v.toObject().value("request")==id)return v.toObject();return QJsonObject{};};
 NetworkWorkView paged([&](int offset,NetworkWorkView::Result done){++seekReads;QJsonArray part;for(int i=offset;i<std::min(offset+100,int(ordered.size()));++i)part.append(ordered[i]);auto result=page;result.insert("leases",part);result.insert("reports",QJsonArray{});result.insert("audits",QJsonArray{});result.insert("active_lease_count",300);done(result,{});},[&](const QString& id,NetworkWorkView::Result done){++remoteDetails;auto result=page;result.insert("leases",QJsonArray{one(id)});result.insert("reports",QJsonArray{});result.insert("audits",QJsonArray{});done(result,{});},nullptr,[](const QString&,const QString&,int,auto done){done({},"Separate atlas transport is outside this page-selection fixture");});
 paged.resize(1366,990);paged.show();app.processEvents();
 auto* selectedTable=paged.findChild<QTableWidget*>("pgn-network-ticket-table");auto* pointCallback=static_cast<pgn::ui::TicketAtlasView*>(paged.findChild<QWidget*>("pgn-ticket-atlas"));need(selectedTable&&pointCallback,"actual table and atlas selection callback available");
 const auto selectPoint=[&](int n){const auto id=QString::number(n,16).rightJustified(64,'0');pointCallback->inspect(id);app.processEvents();QTableWidgetItem* item=nullptr;for(int row=0;row<selectedTable->rowCount();++row)if(selectedTable->item(row,0)->data(Qt::UserRole).toString()==id)item=selectedTable->item(row,0);need(item!=nullptr,"atlas-selected ticket appears on current table page");need(item->isSelected(),"atlas-selected row is highlighted");need(selectedTable->viewport()->rect().intersects(selectedTable->visualItemRect(item)),"selected row is scrolled into the visible viewport");need(paged.property("pgn_selected_ticket_evidence").toJsonObject().value("leases").toArray().first().toObject().value("request")==id,"right evidence and selected row identify the same ticket");};
 selectPoint(250);selectPoint(50);need(seekReads<=6,"page lookup is bounded, not a whole-history walk");paged.close();
 }
 

 { // A late response may have the same selected id but belong to an earlier click.
 QJsonObject lease=leases.first().toObject();
 const auto a=lease.value("request").toString(),b=QString(64,'7');
 QJsonObject thin=page;thin.insert("leases",QJsonArray{lease});thin.insert("reports",QJsonArray{});thin.insert("audits",QJsonArray{});
 thin.insert("height",170);thin.insert("bestblockhash",QString(64,'8'));thin.insert("active_lease_count",1);
 QList<NetworkWorkView::Result> aCalls;
 NetworkWorkView raced([&](int,NetworkWorkView::Result done){done(thin,{});},
     [&](const QString& id,NetworkWorkView::Result done){if(id==a)aCalls.append(std::move(done));},
     nullptr,[](const QString&,const QString&,int,auto done){done({},"Separate atlas data is outside the ordering fixture");});
 raced.resize(1366,850);raced.show();app.processEvents();
 auto* point=static_cast<pgn::ui::TicketAtlasView*>(raced.findChild<QWidget*>("pgn-ticket-atlas"));
 need(point&&aCalls.size()==1,"first point evidence request remains outstanding");
 raced.resize(930,850);app.processEvents();need(aCalls.size()==1,"resize adds zero point evidence reads");
 point->inspect(b);point->inspect(a);app.processEvents();
 need(aCalls.size()==2,"same ticket was selected again after another ticket");
 auto* racedTable=raced.findChild<QTableWidget*>("pgn-network-ticket-table");
 need(racedTable->currentRow()==0&&racedTable->item(0,0)->isSelected(),"current point is visible on the narrower table");
 auto fresh=thin;fresh.insert("height",171);fresh.insert("bestblockhash",QString(64,'9'));
 fresh.insert("reports",QJsonArray{QJsonObject{{"request_id",a},{"block",QString(64,'c')},{"height",171}}});
 aCalls[1](fresh,{});
 auto fromOlderClick=thin;
 fromOlderClick.insert("reports",QJsonArray{QJsonObject{{"request_id",a},{"block",QString(64,'d')},{"height",170}}});
 aCalls[0](fromOlderClick,{});
 const bool keptNewest=raced.property("pgn_selected_ticket_evidence").toJsonObject().value("height")==171;
 QPushButton* open=nullptr;QPushButton* reread=nullptr;
 for(auto* button:raced.findChildren<QPushButton*>()){
     if(button->text()=="Open block")open=button;
     if(button->text()=="Refresh work")reread=button;
 }
 need(open&&reread&&open->isEnabled(),"confirmed block is available on the current evidence");
 lease.remove("report_root");thin.insert("leases",QJsonArray{lease});
 thin.insert("height",172);thin.insert("bestblockhash",QString(64,'a'));
 raced.resize(1366,850);reread->click();app.processEvents();
 const bool clearedBlock=!open->isEnabled();
 std::cout<<"Delayed repeated-selection kept newest: "<<keptNewest<<"; report removal disabled Open block: "<<clearedBlock<<"\n";
 need(keptNewest,"delayed response from before selecting the other point replaced current evidence");
 need(clearedBlock,"report removed on the same point but Open block still uses a previous branch");
 need(racedTable->currentRow()==0&&racedTable->item(0,0)->isSelected(),"same selected row remains visible after reorganization");
 raced.close();
 }

 { // Exhaustive native-atlas projection: all 36 logical slot pairs.
 const QString states[]{"awaiting","assigned","expired","committed","passed","failed"};
 const char* expected[6][6]{
  {"pending_audits","audits_assigned","pending_audits","audits_assigned","audits_assigned","audit_failed"},
  {"audits_assigned","audits_assigned","audits_assigned","audits_assigned","audits_assigned","audit_failed"},
  {"pending_audits","audits_assigned","pending_audits","audits_assigned","audits_assigned","audit_failed"},
  {"audits_assigned","audits_assigned","audits_assigned","audits_assigned","audits_assigned","audit_failed"},
  {"audits_assigned","audits_assigned","audits_assigned","audits_assigned","audits_assigned","audit_failed"},
  {"audit_failed","audit_failed","audit_failed","audit_failed","audit_failed","audit_failed"}};
  for(int a=0;a<6;++a)for(int b=0;b<6;++b){
   QJsonArray jobs;for(const int slot:{a,b})if(states[slot]!="awaiting")jobs.append(QJsonObject{{"state",states[slot]=="expired"?QString("assigned"):states[slot]},{"expired",states[slot]=="expired"}});
   const auto actual=pgn::ui::auditReportPresentation(QJsonObject{{"audited",false},{"jobs",jobs}});
   need(actual.color==pgn::ui::ticketStagePresentation(expected[a][b]).color,"two-slot report projection differs from native atlas state table");
  }
  const auto qualified=pgn::ui::auditReportPresentation(QJsonObject{{"audited",true},{"jobs",QJsonArray{QJsonObject{{"state","passed"}},QJsonObject{{"state","passed"}}}}});
  need(qualified.color==pgn::ui::ticketStagePresentation("audits_passed").color,"qualified pair must use the passed atlas stage");
 }
 const auto captures=qEnvironmentVariable("PGN_AUDIT_UI_CAPTURE");
 if(!captures.isEmpty()){
  QJsonArray captureLeases,captureReports,captureAudits;
  const QString captureStates[6][2]{{"passed","passed"},{"passed","committed"},{"assigned",""},{"failed","passed"},{"committed","committed"},{"assigned","assigned"}};
  for(int i=0;i<6;++i){auto lease=leases[i].toObject();lease.insert("audited",i==0);captureLeases.append(lease);captureReports.append(reports[i]);QJsonArray auditJobs;for(int slot=0;slot<2;++slot)if(!captureStates[i][slot].isEmpty())auditJobs.append(QJsonObject{{"slot",slot},{"attempt",0},{"state",captureStates[i][slot]}});captureAudits.append(QJsonObject{{"request_id",lease.value("request")},{"audited",i==0},{"jobs",auditJobs}});}
  const QJsonObject capturePage{{"leases",captureLeases},{"reports",captureReports},{"audits",captureAudits},{"genesis",QString(64,'a')},{"bestblockhash",QString(64,'7')},{"height",200},{"active_lease_count",6}};
  NetworkWorkView captureView([capturePage](int,NetworkWorkView::Result consume){consume(capturePage,{});},[](const QString&,NetworkWorkView::Result consume){consume({},"Capture page is complete");});captureView.show();app.processEvents();auto* captureTable=captureView.findChild<QTableWidget*>("pgn-network-ticket-table");need(captureTable&&captureTable->rowCount()==6&&captureTable->item(0,4)->text()=="Assigned audits passed","capture contains genuine complete passed row");captureTable->setCurrentCell(0,0);captureTable->selectRow(0);captureTable->scrollToItem(captureTable->item(0,0));
  QDir().mkpath(captures);captureView.setStyleSheet("QMainWindow,QWidget{background:#010b10;color:#c9eee7;}"+HorizonWorkspaceControlStyle());
  for(int width:{1366,1920}){captureView.resize(width,900);for(int n=0;n<8;++n)app.processEvents();need(captureView.grab().save(QDir(captures).filePath(QString("network-work-%1.png").arg(width))),"real themed widget capture");}
 }

 // A real widget displays global scientific coverage independently of this page.
 QJsonObject covFrame=page;covFrame.insert("genesis",QString(64,'1'));covFrame.insert("bestblockhash",QString(64,'a'));covFrame.insert("height",200);
 QJsonObject cov{{"genesis",QString(64,'1')},{"bestblockhash",QString(64,'a')},{"height",200},
  {"origin","150000000000000000000"},{"range_width","200000000000000"},
  {"contiguous_end","150000400000000000000"},{"first_missing_index","2"},
  {"first_missing_end","150000600000000000000"},{"validated_ranges",11324},
  {"last_validated_end","152265000000000000000"}};
 covFrame.insert("scientific_coverage",cov);
 NetworkWorkView covered([&](int,NetworkWorkView::Result done){done(covFrame,{});},[](const QString&,NetworkWorkView::Result done){done({},"No additional details");});
 covered.resize(1366,900);covered.show();app.processEvents();
 auto* covLabel=covered.findChild<QLabel*>("pgn-scientific-coverage");need(covLabel&&covLabel->accessibleDescription().contains("First unvalidated range: 3"),"actual widget shows first scientific hole, not random ticket ID");
 need(covLabel->toolTip().contains("150000400000000000000"),"exact range boundary available in tooltip");
 if(!captures.isEmpty()){
  covered.setStyleSheet("QWidget{background:#010b10;color:#c9eee7;}"+HorizonWorkspaceControlStyle());
  for(int width:{800,1920,1366}){covered.resize(width,900);for(int n=0;n<8;++n)app.processEvents();covered.resize(width,900);for(int n=0;n<8;++n)app.processEvents();const auto image=covered.grab();need(image.width()==width,"capture must have the advertised actual width");need(image.save(QDir(captures).filePath(QString("coverage-%1.png").arg(width))),"qualified coverage widget capture");}
 }
 QPushButton* covRefresh=nullptr;for(auto* button:covered.findChildren<QPushButton*>())if(button->text()=="Refresh work")covRefresh=button;
 need(covRefresh,"coverage refresh action exists");covFrame.insert("bestblockhash",QString(64,'b'));covRefresh->click();app.processEvents();
 need(covLabel->text()=="Scientific coverage unavailable","new branch never keeps stale coverage or invents zero");
 covFrame.remove("scientific_coverage");covRefresh->click();app.processEvents();
 need(covLabel->text()=="Scientific coverage unavailable","older Core without optional summary keeps an honest unavailable state");
 {
  struct Lookup{QString number;NetworkWorkView::Result callback;};
  QVector<Lookup> lookups;
  auto currentPage=page;currentPage.insert("height",200);currentPage.insert("bestblockhash",QString(64,'1'));
  const auto wait=[&]{QEventLoop loop;QTimer::singleShot(260,&loop,&QEventLoop::quit);loop.exec();};
  NetworkWorkView searchable([&](int,NetworkWorkView::Result done){done(currentPage,{});},
    [](const QString&,NetworkWorkView::Result done){done({},"Unexpected extra detail request");});
  searchable.setTicketNumberReader([&](const QString& number,NetworkWorkView::Result done){lookups.append({number,std::move(done)});});
  searchable.resize(1280,800);searchable.show();app.processEvents();
  auto* field=searchable.findChild<QLineEdit*>("pgn-work-ticket-search");
  auto* result=searchable.findChild<QTableWidget*>("pgn-network-ticket-table");
  QPushButton *reload=nullptr,*next=nullptr,*copy=nullptr;
  for(auto* button:searchable.findChildren<QPushButton*>()){
   if(button->text()=="Refresh work")reload=button;
   if(button->text()=="Next page")next=button;
   if(button->text()=="Copy evidence")copy=button;
  }
  need(field&&result&&reload&&next&&copy,"exact-search fixture controls");
  field->setText("ticket 33");
  need(result->rowCount()==1&&result->item(0,0)->text()=="Ticket 33"&&lookups.isEmpty()&&!next->isEnabled(),
       "cached exact lookup did not project one ticket without waiting");
  field->setText("101");wait();
  need(lookups.size()==1&&lookups.first().number=="101"&&result->rowCount()==0&&!copy->isEnabled(),"old details remained active in an exact lookup");
  field->setText("102");wait();
  need(lookups.size()==1,"overlapping exact history calls");
  currentPage.insert("height",201);currentPage.insert("bestblockhash",QString(64,'2'));reload->click();wait();
  QJsonObject obsolete{{"height",200},{"bestblockhash",QString(64,'1')},{"leases",QJsonArray{}}};
  auto old=lookups.takeFirst();old.callback(obsolete,{});
  need(lookups.size()==1&&lookups.first().number=="102"&&result->rowCount()==0&&!copy->isEnabled(),"superseded lookup took over current query");
  auto missing=currentPage;
  for(const auto* key:{"leases","audits","reports"})missing.insert(key,QJsonArray{});
  auto latest=lookups.takeFirst();latest.callback(missing,{});
  need(result->rowCount()==0,"verified negative produced ticket rows");
  reload->click();wait();
  need(lookups.size()==1&&lookups.first().number=="102","Refresh did not retry the same negative lookup");
  auto match=currentPage;
  auto lease=leases.first().toObject();lease.insert("ticket_number","102");
  match.insert("leases",QJsonArray{lease});match.insert("reports",QJsonArray{reports.first()});
  match.insert("audits",QJsonArray{audits.first()});
  auto fresh=lookups.takeFirst();fresh.callback(match,{});
  need(result->rowCount()==1&&result->item(0,0)->text()=="Ticket 102"&&copy->isEnabled(),
       "fresh assignment and its exact evidence did not replace the negative result");
  auto* filter=searchable.findChild<QComboBox*>("pgn-network-ticket-filter");
  filter->setCurrentIndex(1);
  need(result->rowCount()==0&&!copy->isEnabled()&&searchable.property("pgn_selected_ticket_evidence").toJsonObject().isEmpty(),
       "filter hid the result without retiring its evidence actions");
  filter->setCurrentIndex(0);
  need(result->rowCount()==1&&copy->isEnabled(),"clearing exact filter did not restore matching evidence");
 }
 std::cout<<"PASS "<<checks<<" real NetworkWorkView checks;completepage100selections/0detailRPC,partialpageboundedprefetch/exactfallback,stable row+detail identities across advancing anchors,real audit delta,selection and transport recovery,distinct95bitbounds/sharedauditquantity,responsivecompactrows\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
