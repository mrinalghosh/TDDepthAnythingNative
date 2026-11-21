# Downloads a pinned Depth Anything V2 Core ML model from Hugging Face and
# compiles it to .mlmodelc. Provides: DA_MODELC (path) and target `da_model`.

set(DA_MODEL_REPO    "apple/coreml-depth-anything-v2-small")
set(DA_MODEL_REV     "cfef6f6f2a70783dedc0bfae40cecbc2052285d3")  # pinned commit
set(DA_MODEL_VARIANT "DepthAnythingV2SmallF16")
set(DA_MLMODEL_SHA256 "44ac97a3efcfd52113183fb2862ff59cd0368e9ec2e30a90a54980dd11407042")
set(DA_WEIGHTS_SHA256 "fa60d9b6a155734f59029ebb882fd54e549bfaee3539c1a9cbd2cbbab64a0fed")

set(DA_BASE "https://huggingface.co/${DA_MODEL_REPO}/resolve/${DA_MODEL_REV}/${DA_MODEL_VARIANT}.mlpackage")
set(DA_PKG  "${CMAKE_SOURCE_DIR}/models/${DA_MODEL_VARIANT}.mlpackage")

# Download at configure time; skipped when files already exist with matching hashes.
if(NOT EXISTS "${DA_PKG}/Manifest.json")
  file(DOWNLOAD "${DA_BASE}/Manifest.json" "${DA_PKG}/Manifest.json" STATUS st)
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    file(REMOVE "${DA_PKG}/Manifest.json")
    message(FATAL_ERROR "Manifest.json download failed: ${st}")
  endif()
endif()
file(DOWNLOAD "${DA_BASE}/Data/com.apple.CoreML/model.mlmodel"
     "${DA_PKG}/Data/com.apple.CoreML/model.mlmodel"
     EXPECTED_HASH SHA256=${DA_MLMODEL_SHA256})
message(STATUS "Fetching ${DA_MODEL_VARIANT} weights (~50 MB, first run only)")
file(DOWNLOAD "${DA_BASE}/Data/com.apple.CoreML/weights/weight.bin"
     "${DA_PKG}/Data/com.apple.CoreML/weights/weight.bin"
     EXPECTED_HASH SHA256=${DA_WEIGHTS_SHA256} SHOW_PROGRESS)

# Compile .mlpackage -> .mlmodelc at build time.
set(DA_MODELC "${CMAKE_BINARY_DIR}/${DA_MODEL_VARIANT}.mlmodelc")
add_custom_command(
  OUTPUT  "${DA_MODELC}/coremldata.bin"
  COMMAND xcrun coremlcompiler compile "${DA_PKG}" "${CMAKE_BINARY_DIR}"
  DEPENDS "${DA_PKG}/Manifest.json"
          "${DA_PKG}/Data/com.apple.CoreML/model.mlmodel"
          "${DA_PKG}/Data/com.apple.CoreML/weights/weight.bin"
  COMMENT "Compiling ${DA_MODEL_VARIANT} with coremlcompiler"
  VERBATIM)
add_custom_target(da_model ALL DEPENDS "${DA_MODELC}/coremldata.bin")
