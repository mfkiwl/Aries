#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Affine/Utils.h"
#include "mlir/Dialect/Affine/LoopUtils.h"
#include "mlir/Dialect/Affine/Analysis/LoopAnalysis.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/Support/Debug.h"
#include "aries/Transform/Passes.h"
#include "aries/Transform/Utils.h"
#include "aries/Dialect/ADF/ADFDialect.h"

using namespace mlir;
using namespace aries;
using namespace adf;
using namespace mlir::memref;
using namespace mlir::func;
using namespace mlir::affine;

namespace {

struct AriesIOPlacement : public AriesIOPlacementBase<AriesIOPlacement> {
public:
  AriesIOPlacement() = default;
  AriesIOPlacement(const AriesOptions &opts) {
    FirstCol=opts.OptFirstCol;
    NumShim=opts.OptNumShim;
    MidLine=opts.OptMidLine;
    ChalIn=opts.OptChalIn;
    ChalOut=opts.OptChalOut;
    EnableIOCons=opts.OptEnableIOCons;
    EnablePL = opts.OptEnablePL;
    EnableAIE2 = opts.OptEnableAIE2;
  }
  void runOnOperation() override {
    auto mod = dyn_cast<ModuleOp>(getOperation());
    if(!EnablePL && EnableAIE2){
      if (!NPUIOPlacement(mod))
        signalPassFailure();
    }else{
      if (!IOPlacement(mod))
        signalPassFailure();
    }
  }

private:
  bool findCorePlace(Operation* use, Value io, bool& inDir, 
                     int& colInt){
    auto connectOp = dyn_cast<ConnectOp>(use);
    auto src = connectOp.getSrc();
    auto dst = connectOp.getDst();
    int finalCol = 0;
    CallOp call;
    // Send data to IO
    if(src!=io){
      auto defineOp = src.getDefiningOp();
      // For NPU, currently the output won't be returned
      if(!defineOp){
        return false;
      }else if(auto buf = dyn_cast<BufferOp>(defineOp)){
        // Assume src (L2 buffer) will only be used once
        bool break_flag = false;
        for(auto useNew : src.getUsers()){
          if(auto dmaOp = dyn_cast<DmaOp>(useNew)){
            auto srcDma = dmaOp.getSrc();
            for(auto user : srcDma.getUsers()){
              if(!dyn_cast<CallOp>(user)){
                continue;
              }else{
                call = dyn_cast<CallOp>(user);
                inDir = false;
                break_flag = true;
                break;
              }
            }
            if(break_flag)
              break;
          }else if(auto merge = dyn_cast<DmaMergeOp>(useNew)){
            auto mergeSrcs = merge.getSrc();
            auto srcNum = mergeSrcs.size();
            for (auto mergeSrc : mergeSrcs){
              for(auto user : mergeSrc.getUsers()){
                if(!dyn_cast<CallOp>(user)){
                  continue;
                }else{
                  auto newCall = dyn_cast<CallOp>(user);
                  auto corePlaceAttr = dyn_cast<ArrayAttr>(
                                       newCall->getAttr("col, row"));
                  auto colAttr = corePlaceAttr[0];
                  auto intAttr = dyn_cast<IntegerAttr>(colAttr);
                  auto colIntTemp = intAttr.getInt();
                  finalCol += colIntTemp;
                  inDir = false;
                  break_flag = true;
                  break;
                }
              }
            }
            finalCol = std::floor(finalCol/srcNum);
            if(break_flag)
              break;
          }
        }
      }else if(dyn_cast<CallOp>(defineOp)){
        call = dyn_cast<CallOp>(defineOp);
        inDir = false;
      }else{
        return false;
      }
    }else{ // Receive data from IO
      bool break_flag = false;
      for(auto valUse : dst.getUsers()){
        if(auto dmaOp = dyn_cast<DmaOp>(valUse)){
          auto dstDma = dmaOp.getDst();
          for(auto user : dstDma.getUsers()){
            if(!dyn_cast<CallOp>(user)){
              continue;
            }else{
              call = dyn_cast<CallOp>(user);
              inDir = true;
              break_flag = true;
              break;
            }
          }
          if(break_flag)
            break;
        }else if(auto broadcast = dyn_cast<DmaBroadcastOp>(valUse)){
          auto dsts = broadcast.getDst();
          auto dstNum = dsts.size();
          for (auto dstDma : dsts){
            for(auto user : dstDma.getUsers()){
              if(!dyn_cast<CallOp>(user)){
                continue;
              }else{
                auto newCall = dyn_cast<CallOp>(user);
                auto corePlaceAttr = dyn_cast<ArrayAttr>(
                                     newCall->getAttr("col, row"));
                auto colAttr = corePlaceAttr[0];
                auto intAttr = dyn_cast<IntegerAttr>(colAttr);
                auto colIntTemp = intAttr.getInt();
                finalCol += colIntTemp;
                inDir = true;
                break_flag =true;
                break;
              }
            }
          }
          finalCol = std::floor(finalCol/dstNum);
          if(break_flag)
            break;
        }else if(auto callOp = dyn_cast<CallOp>(valUse)){
          call = callOp;
          inDir = true;
          break;
        }
      }
    }
    if(call){ // If the IO is connected to one core
      auto corePlaceAttr = dyn_cast<ArrayAttr>(call->getAttr("col, row"));
      auto colAttr = corePlaceAttr[0];
      auto intAttr = dyn_cast<IntegerAttr>(colAttr);
      colInt = intAttr.getInt();
    }else{ // If the IO is connceted to many cores(Broadcast)
      colInt = finalCol;
    }
    return true;
  }

  bool checkValidChal(unsigned& col, unsigned& chl, int arrayIndex,
                      unsigned colFirst, bool enable, 
                      std::vector<int>& tileChannel){
    auto curChl = tileChannel[arrayIndex];
    if((!enable && curChl>=0) || (enable && curChl>=1)){
      col = arrayIndex + colFirst;
      chl = curChl * 2;
      tileChannel[arrayIndex] = curChl -1;
      return true;
    }
    return false;
  }

  // Here find available channel at the startPos, if not then jump forward and
  // back forward between startPos, if reach one end then only go another side
  bool findPlacement(unsigned colFirst, unsigned numTile, unsigned startPos, 
                     unsigned midLine, bool en, unsigned& col, 
                     unsigned& chl, std::vector<int>& tileChannel){
    unsigned colLast = colFirst + (numTile-1);
    if(startPos < colFirst)
      startPos = colFirst;
    if(startPos > colLast)
      startPos = colLast;
    int firstIndex = startPos - colFirst; // Record start pos of tileChannel
    int arrayIndex = firstIndex; //Used to move position of tileChannel
    bool direction = false; //Towards left
    bool end_flag = false;
    bool reachEnd = false;
    unsigned forwardCnt = 1;
    unsigned backwardCnt = 1;
    while(!end_flag){
      if(checkValidChal(col, chl, arrayIndex, colFirst, en, tileChannel))
        break;
      // If reachEnd been marked, then one side is already full
      // Only search one direction
      if(reachEnd){
        if(direction){
          arrayIndex++;
          if(arrayIndex > (int)(numTile-1))
            return false;
          if(checkValidChal(col, chl, arrayIndex, colFirst, en, tileChannel))
            end_flag = true;
        }else{
          arrayIndex--;
          if(arrayIndex < 0)
            return false;
          if(checkValidChal(col, chl, arrayIndex, colFirst, en, tileChannel))
            end_flag = true;
        }
      }else{
        // If both sides doesn't reach end, alternatively change direction
        if(direction){
          arrayIndex = firstIndex + forwardCnt;
          if(arrayIndex > (int)(numTile-1)){
            reachEnd = true;
            arrayIndex = numTile-1;
            if(checkValidChal(col, chl, arrayIndex, colFirst, en, tileChannel))
              end_flag = true;
            direction = !direction;
          }else{
            if(checkValidChal(col, chl, arrayIndex, colFirst, en, tileChannel))
              end_flag = true;
            forwardCnt++;
            direction = !direction;
          }
        }else{
          arrayIndex = firstIndex - backwardCnt;
          if(arrayIndex < 0){
            reachEnd = true;
            arrayIndex = 0;
            if(checkValidChal(col, chl, arrayIndex, colFirst, en, tileChannel))
              end_flag = true;
            direction = !direction;
          }else{
            if(checkValidChal(col, chl, arrayIndex, colFirst, en, tileChannel))
              end_flag = true;
            backwardCnt++;
            direction = !direction;
          }
        }
      }
    }
    return true;
  }

  bool L2IOPlacement(Operation* use, Value io, 
                     std::vector<int>& tileInChl,
                     std::vector<int>& tileOutChl,
                     int64_t& colInt,
                     int64_t& chlInt){
    auto connectOp = dyn_cast<ConnectOp>(use);
    auto src = connectOp.getSrc();
    auto dst = connectOp.getDst();
    auto mem = src;
    bool inDir = false;
    if(src==io){ // src is IO, means send data in
      mem = dst;
      inDir = true;
    }
    auto memType = dyn_cast<MemRefType>(mem.getType());
    if(!memType){
      llvm::errs() << "Found IO connect to non-mem value\n";
      signalPassFailure();
    }
    auto memSpace = memType.getMemorySpaceAsInt();
    if(memSpace != (int)MemorySpace::L2)
      return false;
    BuffLocOp buffLoc;
    for (auto use : mem.getUsers()) {
      if (auto locOp = dyn_cast<BuffLocOp>(use)) {
        buffLoc = locOp;
        break;
      }
    }
    if(!buffLoc){
      llvm::errs() << "Found unplaced L2 buffer\n";
      signalPassFailure();
    }
    auto col = buffLoc.getCol();
    auto constantOp = col.getDefiningOp<arith::ConstantOp>();
    auto intAttr = dyn_cast<mlir::IntegerAttr>(constantOp.getValue());
    colInt = intAttr.getInt();
    if(inDir){
      chlInt = tileInChl[colInt];
      tileInChl[colInt]--;
      if(chlInt < 0){
        llvm::errs() << "GMIO fails to place under L2 buffer\n";
        signalPassFailure();
      }
    }else{
      chlInt = tileOutChl[colInt];
      tileOutChl[colInt]--;
      if(chlInt < 0){
        llvm::errs() << "GMIO fails to place under L2 buffer\n";
        signalPassFailure();
      }
    }
    return true;
  }

  bool NPUIOPlacement (ModuleOp mod) {
    auto builder = OpBuilder(mod);
    auto indexType = builder.getIndexType();
    unsigned colFirst = FirstCol;
    unsigned numTile = NumShim;
    unsigned midLine = MidLine;
    unsigned numInChl = ChalIn;
    unsigned numOutChl = ChalOut;
    std::vector<int> tileInChl(numTile, numInChl-1);
    std::vector<int> tileOutChl(numTile, numOutChl-1);
    auto flag = mod.walk([&](FuncOp func){
      if(!func->hasAttr("adf.cell"))
        return WalkResult::advance();
      for(auto configOp : func.getOps<ConfigGMIOOp>()){
        SmallVector<unsigned, 4> corePlace;
        SmallVector<int, 4> broadPos;
        auto gmio = configOp.getGmio();
        int disToMid = 0;
        int cnt = 0;
        bool inDir;
        // If IO connect to L2 buffer is placed, then place next IO
        bool break_flag = false;
        for(auto use : gmio.getUsers()){
          if(!dyn_cast<ConnectOp>(use))
            continue;
          // TODO:The code structure should be modified
          // If the GMIO connect to L2 buffer then it should be placed the same
          // col as the L2 buffer
          int64_t colInt, chlInt;
          // If true means already placed for gmio connected to L2 buffer
          if(L2IOPlacement(use, gmio, tileInChl, tileOutChl, colInt, chlInt)){
            auto colAttr = builder.getIntegerAttr(indexType, colInt);
            auto chlAttr = builder.getIntegerAttr(indexType, chlInt);
            auto arrayAttr = builder.getArrayAttr({colAttr, chlAttr});
            configOp->setAttr("col, chl", arrayAttr);
            break_flag = true;
            break;
          }
          int colCore;
          if(!findCorePlace(use, gmio, inDir, colCore))
            return WalkResult::interrupt();
          broadPos.push_back(colCore);
          disToMid += colCore-midLine;
          cnt++;
        }
        if (break_flag)
          continue;
        if(cnt == 0)
          return WalkResult::advance();
        unsigned col;
        unsigned chl;
        int avgToMid = std::ceil(disToMid/(float)cnt);
        unsigned startPos = avgToMid + midLine;
        bool enable = false;
        if(inDir){
          if(!findPlacement(colFirst, numTile, startPos, midLine, enable, 
                            col, chl, tileInChl))
            return WalkResult::interrupt();
        }else{
          if(!findPlacement(colFirst, numTile, startPos, midLine, enable, 
                            col, chl, tileOutChl))
            return WalkResult::interrupt();
        }
        auto colAttr = builder.getIntegerAttr(indexType, col);
        auto chlAttr = builder.getIntegerAttr(indexType, chl);
        auto arrayAttr = builder.getArrayAttr({colAttr, chlAttr});
        configOp->setAttr("col, chl", arrayAttr);
      } 
      return WalkResult::advance();
    });
    if(flag == WalkResult::interrupt())
      return false;
    return true;
  }
  
  bool IOPlacement (ModuleOp mod) {
    auto builder = OpBuilder(mod);
    auto indexType = builder.getIndexType();
    unsigned colFirst = FirstCol;
    unsigned numTile = NumShim;
    unsigned midLine = MidLine;
    unsigned numInChl = ChalIn;
    unsigned numOutChl = ChalOut;
    bool enableIOCons= EnableIOCons;
    std::vector<int> tileInChl(numTile, numInChl-1);
    std::vector<int> tileOutChl(numTile, numOutChl-1);
    if(EnableIOCons){
      tileInChl[0]=0;
      tileInChl[1]=0;
      tileInChl[2]=1;
      tileInChl[36]=1;
      tileInChl[37]=0;
      tileInChl[38]=0;
    }
    auto flag = mod.walk([&](FuncOp func){
      if(!func->hasAttr("adf.cell"))
        return WalkResult::advance();
      for(auto configOp : func.getOps<ConfigPLIOOp>()){
        SmallVector<unsigned, 4> corePlace;
        SmallVector<int, 4> broadPos;
        auto plio = configOp.getPlio();
        int disToMid = 0;
        int cnt = 0;
        bool inDir;
        for(auto use : plio.getUsers()){
          if(!dyn_cast<ConnectOp>(use))
            continue;
          int colCore;
          if(!findCorePlace(use, plio, inDir, colCore))
            return WalkResult::interrupt();
          broadPos.push_back(colCore);
          disToMid += colCore-midLine;
          cnt++;
        }
        if(cnt == 0)
          return WalkResult::advance();
        unsigned col;
        unsigned chl;
        int avgToMid = std::ceil(disToMid/(float)cnt);
        unsigned startPos = avgToMid + midLine;
        // when enableIOCons is enabled check if the variance of the position
        // of the broadcast ops is too small (<1) then limit the number used in
        // this shim
        bool enable = false;
        if(cnt>1 && enableIOCons){
          // Calculate the mean
          double sum = std::accumulate(broadPos.begin(), broadPos.end(), 0.0);
          double mean = sum / cnt;
          // Calculate the variance
          double variance = 0.0;
          for (int num : broadPos)
            variance += (num - mean) * (num - mean);
          variance /= cnt;
          // Calculate the standard deviation
          auto sdvar = std::sqrt(variance);
          if (sdvar < 1.0){
            enable = true;
          }
        }
        if(inDir){
          if(!findPlacement(colFirst, numTile, startPos, midLine, enable, 
                            col, chl, tileInChl))
            return WalkResult::interrupt();
        }else{
          if(!findPlacement(colFirst, numTile, startPos, midLine, enable, 
                            col, chl, tileOutChl))
            return WalkResult::interrupt();
        }
        auto colAttr = builder.getIntegerAttr(indexType, col);
        auto chlAttr = builder.getIntegerAttr(indexType, chl);
        auto arrayAttr = builder.getArrayAttr({colAttr, chlAttr});
        configOp->setAttr("col, chl", arrayAttr);
      } 
      return WalkResult::advance();
    });
    if(flag == WalkResult::interrupt())
      return false;
    return true;
  }

};
} // namespace



namespace mlir {
namespace aries {

std::unique_ptr<Pass> createAriesIOPlacementPass() {
  return std::make_unique<AriesIOPlacement>();
}

std::unique_ptr<Pass> createAriesIOPlacementPass(const AriesOptions &opts) {
  return std::make_unique<AriesIOPlacement>(opts);
}

} // namespace aries
} // namespace mlir