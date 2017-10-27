.SECONDEXPANSION:
all::
SRC=../src

BUILDS= normal gc64 dualnum nojit
normal : XCFLAGS= 
gc64 : XCFLAGS=-DLUAJIT_ENABLE_GC64
dualnum : XCFLAGS=-DLUAJIT_NUMMODE=2
nojit : XCFLAGS=-DLUAJIT_DISABLE_JIT

SRC_LUA= $(wildcard $(SRC)/jit/*.lua)
#Skip the dynamically generated VM info file that is generated at compile time
SRC_LUA:= $(filter-out vmdef.lua,$(SRC_LUA))
LUA_NAMES= $(notdir $(SRC_LUA))

define make_buildtarget
  $1_BUILD_OUTPUTS= builds/$1/libluajit.so builds/$1/luajit builds/$1/jit/vmdef.lua
  $1_LUAFILES= $(addprefix builds/$1/jit/,$(LUA_NAMES))
  $$($1_LUAFILES): $1.copylua
  $$($1_BUILD_OUTPUTS): $1.build
  $1: $$($1_BUILD_OUTPUTS) $$($1_LUAFILES)
  $1: BUILD_TARGET=$1
  BUILD_TARGETS += $1.build
  LUA_TARGETS += $1.copylua
  TEST_TARGETS += $1.test
  CLEAN_TARGETS += $1.clean
endef

$(foreach build, $(BUILDS), $(eval $(call make_buildtarget,$(build))))

.INTERMEDIATE: $(BUILD_TARGETS) $(LUA_TARGETS)

%.copylua: $(SRC_LUA)
	mkdir -p builds/$*/jit/
	$(foreach luafile, $(SRC_LUA), $(eval $(shell  cp $(luafile) builds/$*/jit/)))

%.build: $(SRC)/*.h $(SRC)/*.c
	@echo "==== Building LuaJIT target = ${BUILD_TARGET} ===="
	$(MAKE) -C $(SRC) clean
	$(MAKE) -C $(SRC) -j XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT ${XCFLAGS}"
	mkdir -p builds/${BUILD_TARGET}/
	cp $(SRC)/luajit builds/${BUILD_TARGET}/
	cp $(SRC)/libluajit.so builds/${BUILD_TARGET}/
	mkdir -p builds/${BUILD_TARGET}/jit/
	cp $(SRC)/jit/vmdef.lua builds/${BUILD_TARGET}/jit/
	mkdir -p builds/${BUILD_TARGET}/jitlog/
	cp $(SRC)/jitlog/*.lua builds/${BUILD_TARGET}/jitlog/
	@echo "==== Successfully built LuaJIT ===="

build : $(BUILDS)

all test:: $(TEST_TARGETS) 

#Builds the binaries if they don't exist first
%.test: $$* 
	(cd builds/$* && ./luajit jitlog/test.lua)

%.clean:
	rm -f ./builds/$*/luajit
	rm -f ./builds/$*/libluajit.so
	rm -f ./builds/$*/jit/*.lua

clean: $(CLEAN_TARGETS)
	$(MAKE) -C $(SRC) clean

.PHONY: all build test clean $(BUILDS)
