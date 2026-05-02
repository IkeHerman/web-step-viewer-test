PREFIX ?= /opt/homebrew/opt/opencascade
CXX ?= clang++

SRCDIR := .
DEPDIR := $(SRCDIR)/dep
BUILD_DIR := build
TARGET := model2tile

# ---- Includes ----
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra \
	-isystem $(PREFIX)/include/opencascade \
	-isystem $(DEPDIR)/tinygltf \
	-isystem $(DEPDIR)/meshoptimizer/src

DEPFLAGS := -MMD -MP

# Silence common stb header warning from tinygltf deps
CXXFLAGS += -Wno-missing-field-initializers

# ---- Linker / OCCT libs ----
LDFLAGS ?= -L$(PREFIX)/lib
ASSIMP_INCLUDE_DIR :=
ASSIMP_LIB_DIR :=
ASSIMP_INCLUDE_DIR_2 :=
ifneq ("$(wildcard $(DEPDIR)/assimp/install/include/assimp/Importer.hpp)","")
ASSIMP_INCLUDE_DIR := $(DEPDIR)/assimp/install/include
ASSIMP_LIB_DIR := $(DEPDIR)/assimp/install/lib
else ifneq ("$(wildcard $(DEPDIR)/assimp/include/assimp/Importer.hpp)","")
ifneq ("$(wildcard $(DEPDIR)/assimp/build/bin/libassimp.dylib)","")
ASSIMP_INCLUDE_DIR := $(DEPDIR)/assimp/include
ASSIMP_INCLUDE_DIR_2 := $(DEPDIR)/assimp/build/include
ASSIMP_LIB_DIR := $(DEPDIR)/assimp/build/bin
endif
else ifneq ("$(wildcard /opt/homebrew/opt/assimp/include/assimp/Importer.hpp)","")
ASSIMP_INCLUDE_DIR := /opt/homebrew/opt/assimp/include
ASSIMP_LIB_DIR := /opt/homebrew/opt/assimp/lib
endif

ASSIMP_CXXFLAGS :=
ASSIMP_LDFLAGS :=
ASSIMP_LIBS :=
ASSIMP_RPATH :=
ifneq ("$(ASSIMP_INCLUDE_DIR)","")
ASSIMP_CXXFLAGS := -isystem $(ASSIMP_INCLUDE_DIR)
ifneq ("$(ASSIMP_INCLUDE_DIR_2)","")
ASSIMP_CXXFLAGS += -isystem $(ASSIMP_INCLUDE_DIR_2)
endif
endif
ifneq ("$(ASSIMP_LIB_DIR)","")
ASSIMP_LDFLAGS := -L$(ASSIMP_LIB_DIR)
ASSIMP_LIBS := -lassimp
ASSIMP_RPATH := -Wl,-rpath,$(abspath $(ASSIMP_LIB_DIR))
endif

CXXFLAGS += $(ASSIMP_CXXFLAGS)
LDFLAGS += $(ASSIMP_LDFLAGS)
LIBS ?= -lTKernel -lTKStd -lTKStdL -lTKXSBase -lTKLCAF -lTKDE -lTKDEGLTF -lTKRWMesh -lTKMesh -lTKBRep -lTKPrim -lTKCDF -lTKDCAF -lTKDESTEP -lTKGeomAlgo -lTKGeomBase -lTKMath -lTKService -lTKTopAlgo -lTKXCAF -lTKG2d -lTKG3d -lTKShHealing $(ASSIMP_LIBS)
RPATH := -Wl,-rpath,$(PREFIX)/lib $(ASSIMP_RPATH)

# ---- Project sources ----
SOURCES := \
	$(SRCDIR)/app_main.cpp \
	$(SRCDIR)/step_pipeline.cpp \
	$(SRCDIR)/app/cli_options.cpp \
	$(SRCDIR)/pipeline/pipeline_runner.cpp \
	$(SRCDIR)/importers/step_importer.cpp \
	$(SRCDIR)/importers/step_instance_lod.cpp \
	$(SRCDIR)/importers/step_pipeline_support.cpp \
	$(SRCDIR)/importers/step_traversal.cpp \
	$(SRCDIR)/importers/fbx_importer.cpp \
	$(SRCDIR)/importers/fbx_traversal.cpp \
	$(SRCDIR)/importers/fbx_instance_lod.cpp \
	$(SRCDIR)/adapters/step_to_scene_ir.cpp \
	$(SRCDIR)/adapters/fbx_to_scene_ir.cpp \
	$(SRCDIR)/fbx_pipeline.cpp \
	$(SRCDIR)/tiler/scene_ir_tile_items.cpp \
	$(SRCDIR)/octree.cpp \
	$(SRCDIR)/importers/step_glb_export.cpp \
	$(SRCDIR)/b3dm.cpp \
	$(SRCDIR)/tileset_emit.cpp \
	$(SRCDIR)/glb_compose_instancing.cpp \
	$(SRCDIR)/tinygltf_imp.cpp \
	$(SRCDIR)/glbopt.cpp \
	$(SRCDIR)/glbopt_deep_copy.cpp \
	$(SRCDIR)/glbopt_io.cpp \
	$(SRCDIR)/glbopt_optimize.cpp \
	$(SRCDIR)/glbopt_rewrite.cpp \
	$(SRCDIR)/stpsani.cpp

# ---- meshoptimizer sources (split across many .cpp files) ----
MESHOPT_DIR := $(DEPDIR)/meshoptimizer/src
MESHOPT_SOURCES := $(wildcard $(MESHOPT_DIR)/*.cpp)
MESHOPT_OBJECTS := $(patsubst $(MESHOPT_DIR)/%.cpp,$(BUILD_DIR)/meshopt_%.o,$(MESHOPT_SOURCES))

# ---- Objects ----
PROJECT_OBJECTS := $(patsubst $(SRCDIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
OBJECTS := $(PROJECT_OBJECTS) $(MESHOPT_OBJECTS)
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean
all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

# Compile your sources
$(BUILD_DIR)/%.o: $(SRCDIR)/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile meshoptimizer sources into build/meshopt_*.o
$(BUILD_DIR)/meshopt_%.o: $(MESHOPT_DIR)/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Link
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LIBS) $(RPATH) -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)
