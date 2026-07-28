# Bitflash Linux build — ImGui + GLFW + OpenGL3
# Called by: make linux (root Makefile)

CXX ?= g++

SECP_INC    ?= /usr/local/include
SECP_LIB    ?= /usr/local/lib
RANDOMX_DIR ?= $(HOME)/RandomX

IMGUI_DIR = imgui

CXXFLAGS = -std=gnu++14 -O2 -w -fpermissive -pthread \
           -DSECP256K1_STATIC \
           -I$(SECP_INC) -I$(RANDOMX_DIR)/src \
           -I/usr/include/nlohmann \
           -I$(IMGUI_DIR)

LIBS = -L$(SECP_LIB) -lsecp256k1 \
       -L$(RANDOMX_DIR)/build -lrandomx \
       -lsodium -lssl -lcrypto -ldb_cxx -lpthread \
       -lglfw -lGL

HEADERS = headers_core.h headers.h util.h main.h serialize.h uint256.h \
          key.h bignum.h script.h db.h base58.h net.h nostr.h \
          randomx_pow.h btfaddr.h btfchan.h btfrv.h btftunnel.h market.h compat.h

IMGUI_OBJS = obj/imgui.o obj/imgui_draw.o obj/imgui_tables.o \
             obj/imgui_widgets.o obj/imgui_impl_glfw.o obj/imgui_impl_opengl3.o

NODE_OBJS = obj/util.o obj/script.o obj/db.o obj/net.o obj/main.o \
            obj/market.o obj/sha.o obj/nostr.o obj/randomx_pow.o \
            obj/btfaddr.o obj/btfchan.o obj/btfrv.o obj/btftunnel.o obj/rpc.o

GUI_OBJS = obj/gui.o obj/main_gui.o

all: bitflash

obj/%.o: %.cpp $(HEADERS)
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) -o $@ $<

obj/sha.o: sha.cpp sha.h
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) -O3 -o $@ $<

obj/imgui.o:              $(IMGUI_DIR)/imgui.cpp
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) -o $@ $<

obj/imgui_draw.o:         $(IMGUI_DIR)/imgui_draw.cpp
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) -o $@ $<

obj/imgui_tables.o:       $(IMGUI_DIR)/imgui_tables.cpp
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) -o $@ $<

obj/imgui_widgets.o:      $(IMGUI_DIR)/imgui_widgets.cpp
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) -o $@ $<

obj/imgui_impl_glfw.o:    $(IMGUI_DIR)/imgui_impl_glfw.cpp
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) -o $@ $<

obj/imgui_impl_opengl3.o: $(IMGUI_DIR)/imgui_impl_opengl3.cpp
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) -o $@ $<

bitflash: $(IMGUI_OBJS) $(NODE_OBJS) $(GUI_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
	@echo "Built: bitflash"

clean:
	rm -rf obj bitflash
