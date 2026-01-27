PREFIX ?= /opt/homebrew/opt/opencascade
CXX ?= clang++

SRCDIR := .
DEPDIR := $(SRCDIR)/dep
BUILD_DIR := build
TARGET := stp2tile

# ---- Includes ----
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra \
	-I$(PREFIX)/include/opencascade \
	-isystem $(DEPDIR)/tinygltf \
	-isystem $(DEPDIR)/meshoptimizer/src

# Silence common stb header warning from tinygltf deps
CXXFLAGS += -Wno-missing-field-initializers

# ---- Linker / OCCT libs ----
LDFLAGS ?= -L$(PREFIX)/lib
LIBS ?= -lTKernel -lTKStd -lTKStdL -lTKLCAF -lTKDE -lTKDEGLTF -lTKRWMesh -lTKMesh -lTKBRep -lTKPrim -lTKCDF -lTKDCAF -lTKDESTEP -lTKGeomAlgo -lTKGeomBase -lTKMath -lTKService -lTKTopAlgo -lTKXCAF -lTKG2d -lTKG3d
RPATH := -Wl,-rpath,$(PREFIX)/lib

# ---- Project sources ----
SOURCES := \
	$(SRCDIR)/main.cpp \
	$(SRCDIR)/octree.cpp \
	$(SRCDIR)/export_glb.cpp \
	$(SRCDIR)/b3dm.cpp \
	$(SRCDIR)/tileset_emit.cpp \
	$(SRCDIR)/tinygltf_imp.cpp \
	$(SRCDIR)/glbopt.cpp

# ---- meshoptimizer sources (split across many .cpp files) ----
MESHOPT_DIR := $(DEPDIR)/meshoptimizer/src
MESHOPT_SOURCES := $(wildcard $(MESHOPT_DIR)/*.cpp)
MESHOPT_OBJECTS := $(patsubst $(MESHOPT_DIR)/%.cpp,$(BUILD_DIR)/meshopt_%.o,$(MESHOPT_SOURCES))

# ---- Objects ----
OBJECTS := \
	$(BUILD_DIR)/main.o \
	$(BUILD_DIR)/octree.o \
	$(BUILD_DIR)/export_glb.o \
	$(BUILD_DIR)/b3dm.o \
	$(BUILD_DIR)/tileset_emit.o \
	$(BUILD_DIR)/tinygltf_imp.o \
	$(BUILD_DIR)/glbopt.o \
	$(MESHOPT_OBJECTS)

.PHONY: all clean
all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

# Compile your sources
$(BUILD_DIR)/%.o: $(SRCDIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile meshoptimizer sources into build/meshopt_*.o
$(BUILD_DIR)/meshopt_%.o: $(MESHOPT_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LIBS) $(RPATH) -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
