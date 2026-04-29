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
LIBS ?= -lTKernel -lTKStd -lTKStdL -lTKXSBase -lTKLCAF -lTKDE -lTKDEGLTF -lTKRWMesh -lTKMesh -lTKBRep -lTKPrim -lTKCDF -lTKDCAF -lTKDESTEP -lTKGeomAlgo -lTKGeomBase -lTKMath -lTKService -lTKTopAlgo -lTKXCAF -lTKG2d -lTKG3d
RPATH := -Wl,-rpath,$(PREFIX)/lib

# ---- Project sources ----
SOURCES := \
	$(SRCDIR)/app_main.cpp \
	$(SRCDIR)/main.cpp \
	$(SRCDIR)/app/cli_options.cpp \
	$(SRCDIR)/pipeline/pipeline_runner.cpp \
	$(SRCDIR)/importers/step_importer.cpp \
	$(SRCDIR)/importers/fbx_importer.cpp \
	$(SRCDIR)/adapters/step_to_scene_ir.cpp \
	$(SRCDIR)/tiler/scene_ir_tile_items.cpp \
	$(SRCDIR)/exporters/occt_tile_content_baker.cpp \
	$(SRCDIR)/octree.cpp \
	$(SRCDIR)/export_glb.cpp \
	$(SRCDIR)/b3dm.cpp \
	$(SRCDIR)/tileset_emit.cpp \
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
