FUNC:=ttmc
MLIR:=aries
KERNEL_NAME:=kernel_${FUNC}
PROJECT_PATH=./project
PROJECT_AIE_PATH=${PROJECT_PATH}/aie
PROJECT_PL_PATH=${PROJECT_PATH}/kernel
PROJECT_Host_PATH=${PROJECT_PATH}/host

ColNum = 50
RowNum = 8
ColOffset = 0 
RowOffset = 0
ColGap = 0
FirstCol = 6
NumShim = 39
MidLine = 24
ChalIn = 3
ChalOut = 3
CoreAlgo = 2
EnableIOCons = "true"
EN_Link = "true"
AIEUnroll = 1
L1_0 = 1
L2_0 = 1
L1_1 = 2
L2_1 = 2
L1_2 = 2
L2_2 = 2
L1_3 = 1
L2_3 = 2
L1_4 = 2
L2_4 = 1
L1_Tiling = ${L1_0},${L1_1},${L1_2},${L1_3},${L1_4}
L2_Tiling = ${L2_0},${L2_1},${L2_2},${L2_3},${L2_4}
EN_Tiling = "true"
BUFF_0 = 0
BUFF_1 = 1
BUFF_2 = 0
BUFF_3 = 1
# 1:BRAM, 0:URAM
buf_sel = ${BUFF_0},${BUFF_1},${BUFF_2},${BUFF_3}
PLIOWidth = 128
AXIWidth = 512
EN_PL = "true"
EN_AIE2 = "false"
EN_SERIAL = "false"
ifeq ($(EN_AIE2), "true")
	IOType=AIE2_GMIO
else ifeq ($(EN_PL), "true")
	IOType=PLIO
else
	IOType=GMIO
endif

all: pass translate copyfile printcommand

printcommand:
	@echo "PLIOWidth=${PLIOWidth}, AXIWidth=${AXIWidth}, ColNum=${ColNum}, RowNum=${RowNum}, ColOffset=${ColOffset}, RowOffset=${RowOffset}, ColGap=${ColGap}, CoreAlgo=${CoreAlgo}, FirstCol=${FirstCol}, NumShim=${NumShim}, MidLine=${MidLine}, ChalIn=${ChalIn}, ChalOut=${ChalOut}, EnableIOCons=${EnableIOCons}, EN_Link=${EN_Link}, l1-tile-sizes=${L1_Tiling}, l2-tile-sizes=${L2_Tiling}, l3-tile-sizes=${L3_Tiling}, buf_sel=${buf_sel}, EN_PL=${EN_PL}, EN_AIE2=${EN_AIE2}, EN_SERIAL=${EN_SERIAL}, IOType=${IOType}" >${PROJECT_PATH}/command

ARIES_OPTIONS=  tile-func-name=${FUNC}
ARIES_OPTIONS+= l1-tile-sizes=${L1_Tiling} l2-tile-sizes=${L2_Tiling} en-newtiling=${EN_Tiling}
ARIES_OPTIONS+= port-type=${IOType}
ARIES_OPTIONS+= plio-width=${PLIOWidth}
ARIES_OPTIONS+= en-pl=${EN_PL} en-aie2=${EN_AIE2}
ARIES_OPTIONS+= en-link=${EN_Link}
ARIES_OPTIONS+= col-num=${ColNum} row-num=${RowNum} col-offset=${ColOffset} row-offset=${RowOffset} core-gap=${ColGap} core-algo=${CoreAlgo}
ARIES_OPTIONS+= first-col=${FirstCol} num-shim=${NumShim} mid-line=${MidLine} chal-in=${ChalIn} chal-out=${ChalOut} iocons=${EnableIOCons}
ARIES_OPTIONS+= buf-sels=${buf_sel}
ARIES_OPTIONS+= axi-width=${AXIWidth}
ARIES_OPTIONS+= en-serial=${EN_SERIAL}

PIPELINE_OP=-aries-pipeline-versal
pass: ${PROJECT_PATH}/${FUNC}.adf.mlir
${PROJECT_PATH}/${FUNC}.adf.mlir: ${MLIR}.mlir
	aries-opt -o $@ $< \
			${PIPELINE_OP}="${ARIES_OPTIONS}"

ifeq ($(EN_PL),"true")
  ifeq ($(EN_AIE2),"false")
    ifeq ($(EN_Link), "true")
  	  translate: file_split clean_temp
  	else
  	  translate: file_split clean_temp kernel_file_split clean_kernel_vec
  	endif
  else
    # EN_AIE2 is not false
    translate:
  endif
else
  # EN_PL is not true
  translate:
endif

file_split: tempfile
	aries-opt <${MLIR}.mlir \
		-aries-file-split="inputfile-name=temp.cpp path-name=${PROJECT_AIE_PATH}/" \
		>/dev/null
tempfile: ${PROJECT_AIE_PATH}/temp.cpp
${PROJECT_AIE_PATH}/temp.cpp: ${PROJECT_PATH}/${FUNC}.adf.mlir
	aries-translate -o $@ $< \
		-emit-aries-kernels

copyfile:
ifeq ($(EN_PL), "true")
	@if [ -f ${PROJECT_AIE_PATH}/host.cpp ]; then mv ${PROJECT_AIE_PATH}/host.cpp ${PROJECT_Host_PATH}; fi
	@if [ -f ${PROJECT_AIE_PATH}/hls.cpp ]; then mv ${PROJECT_AIE_PATH}/hls.cpp ${PROJECT_PL_PATH}; fi
	@if [ -f ${PROJECT_AIE_PATH}/system.cfg ]; then mv ${PROJECT_AIE_PATH}/system.cfg ${PROJECT_PATH}; fi
endif

kernel_split: ${PROJECT_PATH}/${KERNEL_NAME}.mlir
${PROJECT_PATH}/${KERNEL_NAME}.mlir: ${PROJECT_PATH}/${FUNC}.adf.mlir
	aries-opt -o $@ $< \
		-aries-kernel-split

kernel_vec: ${PROJECT_PATH}/auto_${KERNEL_NAME}_aie.mlir
${PROJECT_PATH}/auto_${KERNEL_NAME}_aie.mlir: ${PROJECT_PATH}/${KERNEL_NAME}_vector.mlir
	aries-opt -o $@ $< \
		--aie-vectorize \
		-unaligned-loads-check=false \
		-canonicalize -cse
${PROJECT_PATH}/${KERNEL_NAME}_vector.mlir: ${PROJECT_PATH}/${KERNEL_NAME}_unroll.mlir
	aries-opt -o $@ $< \
		--affine-super-vectorize="virtual-vector-size=8 vectorize-reductions" \
		--canonicalize
${PROJECT_PATH}/${KERNEL_NAME}_unroll.mlir: ${PROJECT_PATH}/${KERNEL_NAME}.mlir
	aries-opt -o $@ $< \
		--affine-loop-unroll="unroll-factor=${AIEUnroll}" \
		--canonicalize

kernel_file_split: kernel_cc
	aries-opt <${MLIR}.mlir \
		-aries-file-split="inputfile-name=temp.cc path-name=${PROJECT_AIE_PATH}/" \
		>/dev/null
kernel_cc: ${PROJECT_AIE_PATH}/temp.cc
${PROJECT_AIE_PATH}/temp.cc: ${PROJECT_PATH}/auto_${KERNEL_NAME}_aie.mlir
	aries-translate -o $@ $< \
		-emit-kenrel-func

clean_kernel_vec:
	rm -r ${PROJECT_PATH}/${KERNEL_NAME}_vector.mlir ${PROJECT_PATH}/${KERNEL_NAME}_unroll.mlir ${PROJECT_PATH}/${KERNEL_NAME}.mlir

clean_temp:
	rm -r ${PROJECT_AIE_PATH}/temp.cpp

clean_translate:
	rm -r ${PROJECT_AIE_PATH}/adf_graph.cpp ${PROJECT_AIE_PATH}/adf_kernel.h ${PROJECT_AIE_PATH}/adf_graph.h ${PROJECT_PATH}/kernel/hls.cpp ${PROJECT_PATH}/host/host.cpp ${PROJECT_PATH}/system.cfg

clean_pass:
	rm -r ${PROJECT_PATH}/${FUNC}.adf.mlir

clean: clean_pass clean_translate clean_temp clean_kernel_vec