#include "validated_range_coverage.h"
#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#ifdef NDEBUG
#undef NDEBUG // This standalone qualification must keep its checks in Release.
#endif
#include <cassert>
#include <chrono>
#include <iostream>
#include <random>

using Integer = boost::multiprecision::cpp_int;
using Ranges = std::vector<std::pair<Integer,Integer>>;
using pgn::core::BuildValidatedRangeCoverage;

int main()
{
    const Integer origin{"150000000000000000000"}, width{"200000000000000"};
    auto range=[&](uint64_t i){return std::pair<Integer,Integer>{origin+i*width, origin+(i+1)*width};};
    auto empty=BuildValidatedRangeCoverage(origin,width,Ranges{});
    assert(empty && empty->contiguous_end==origin && empty->validated_ranges==0 && !empty->last_validated_end);
    // Robert's example: many later results cannot conceal an early hole.
    Ranges sparse{range(0),range(1)};
    for(uint64_t i=3;i<11325;++i)sparse.push_back(range(i));
    auto before=BuildValidatedRangeCoverage(origin,width,sparse);
    assert(before && before->first_missing_index==2 && before->validated_ranges==11324);
    sparse.push_back(range(2));
    auto after=BuildValidatedRangeCoverage(origin,width,sparse);
    assert(after && after->first_missing_index==11325 && after->validated_ranges==11325);
    // Duplicate reports/reissues and arbitrary arrival order are immaterial.
    sparse.push_back(range(2));sparse.push_back(range(100));
    std::mt19937 rng{259};std::shuffle(sparse.begin(),sparse.end(),rng);
    auto shuffled=BuildValidatedRangeCoverage(origin,width,sparse);
    assert(shuffled && shuffled->contiguous_end==after->contiguous_end && shuffled->validated_ranges==after->validated_ranges);
    // A new branch with the same count but a different first hole must move
    // the frontier backwards. No counter-only cache or monotonic shortcut.
    Ranges branchA{range(0),range(1),range(3)},branchB{range(0),range(2),range(3)};
    assert(BuildValidatedRangeCoverage(origin,width,branchA)->first_missing_index==2);
    assert(BuildValidatedRangeCoverage(origin,width,branchB)->first_missing_index==1);
    assert(!BuildValidatedRangeCoverage(origin,Integer{0},Ranges{}));
    assert(!BuildValidatedRangeCoverage(origin,width,Ranges{{origin-1,origin-1+width}}));
    assert(!BuildValidatedRangeCoverage(origin,width,Ranges{{origin+1,origin+1+width}}));
    assert(!BuildValidatedRangeCoverage(origin,width,Ranges{{origin,origin+width-1}}));
    // Larger-than-machine-integer inputs and endpoints remain exact.
    const Integer huge=Integer{1}<<180;
    auto large=BuildValidatedRangeCoverage(huge,width,Ranges{{huge,huge+width}});
    assert(large && large->contiguous_end==huge+width && large->first_missing_index==1);
    for(int trial=0;trial<1000;++trial){
        Ranges observed;std::vector<bool> oracle(64,false);
        for(int j=0;j<100;++j){const auto i=rng()%64;observed.push_back(range(i));oracle[i]=true;}
        std::shuffle(observed.begin(),observed.end(),rng);
        uint64_t frontier=0;while(frontier<64&&oracle[frontier])++frontier;
        const auto actual=BuildValidatedRangeCoverage(origin,width,observed);
        assert(actual && actual->first_missing_index==frontier &&
               actual->validated_ranges==static_cast<uint64_t>(std::count(oracle.begin(),oracle.end(),true)));
    }
    Ranges many;many.reserve(50000);
    for(uint64_t i=0;i<50000;++i)many.push_back(range(i));
    std::shuffle(many.begin(),many.end(),rng);
    const auto start=std::chrono::steady_clock::now();
    auto measured=BuildValidatedRangeCoverage(origin,width,many);
    const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    assert(measured && measured->first_missing_index==50000 && measured->validated_ranges==50000);
    std::cout<<"{\"PASS\":true,\"ranges\":50000,\"elapsed_us\":"<<us
             <<",\"first_hole_before\":2,\"frontier_after\":11325,\"reorg_and_duplicates_checked\":true}\n";
}
