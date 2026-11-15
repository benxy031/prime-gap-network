#include "scientific_coverage_presentation.h"
#include <QCoreApplication>
#include <boost/multiprecision/cpp_int.hpp>
#include <iostream>
#include <stdexcept>
int main(int argc,char**argv){
 QCoreApplication app(argc,argv);int checks=0;
 const auto need=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Coverage presentation invariant failed");};
 using Integer=boost::multiprecision::cpp_int;
 const Integer origin{"150000000000000000000"},width{"200000000000000"};
 const auto dec=[](const Integer& x){return QString::fromStdString(x.str());};
 QJsonObject frame{{"genesis",QString(64,'1')},{"bestblockhash",QString(64,'a')},{"height",200}};
 QJsonObject coverage{{"genesis",frame.value("genesis")},{"bestblockhash",frame.value("bestblockhash")},{"height",200},
  {"origin",dec(origin)},{"range_width",dec(width)},{"contiguous_end",dec(origin+width*2)},
  {"first_missing_index","2"},{"first_missing_end",dec(origin+width*3)},
  {"validated_ranges",11324},{"last_validated_end",dec(origin+width*11325)}};
 frame.insert("scientific_coverage",coverage);
 auto shown=pgn::ui::scientificCoveragePresentation(frame);
 need(shown.available);need(shown.text.contains("11,324"));need(shown.text.contains("Consecutive from origin: 2"));
 need(shown.text.contains("First unvalidated range: 3"));need(shown.tooltip.contains(dec(origin+width*2)));
 auto absent=frame;absent.remove("scientific_coverage");need(!pgn::ui::scientificCoveragePresentation(absent).available);
 auto stale=coverage;stale.insert("bestblockhash",QString(64,'b'));frame.insert("scientific_coverage",stale);
 need(!pgn::ui::scientificCoveragePresentation(frame).available);
 auto bad=coverage;bad.insert("contiguous_end",dec(origin+width));frame.insert("scientific_coverage",bad);
 need(!pgn::ui::scientificCoveragePresentation(frame).available);
 bad=coverage;bad.insert("first_missing_index",2);frame.insert("scientific_coverage",bad);
 need(!pgn::ui::scientificCoveragePresentation(frame).available);
 auto zero=coverage;zero.insert("contiguous_end",dec(origin));zero.insert("first_missing_index","0");
 zero.insert("first_missing_end",dec(origin+width));zero.insert("validated_ranges",0);zero.remove("last_validated_end");
 frame.insert("scientific_coverage",zero);shown=pgn::ui::scientificCoveragePresentation(frame);
 need(shown.available&&shown.text.contains("Validated ranges: 0")&&shown.text.contains("First unvalidated range: 1"));
 std::cout<<"{\"PASS\":true,\"checks\":"<<checks<<",\"no_fake_zero\":true,\"branch_identity_checked\":true}\n";
}
