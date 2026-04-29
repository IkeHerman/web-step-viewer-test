#pragma once

#include <string>

/**
 * @brief Sanitizes a STEP file to improve robustness for downstream processing.
 *
 * This function performs a conservative preprocessing pass to correct or remove
 * certain malformed or non-conforming STEP constructs that may cause issues
 * during import (e.g., with OCCT STEP/CAF readers).
 *
 * Current behavior includes:
 *
 * 1. Removing invalid APPLIED_GROUP_ASSIGNMENT entries where the items field is `$`
 *    (expected to be a non-empty aggregate).
 *
 * 2. Patching GROUP_RELATIONSHIP($,$,...) to GROUP_RELATIONSHIP('','',...)
 *    where optional string fields are missing.
 *
 * The function writes a sanitized STEP file and outputs debug logging describing
 * any modifications made.
 *
 * @param inputPath  Path to the original STEP file
 * @param outputPath Path where the sanitized STEP file will be written
 *
 * @return true if successful, false on file I/O failure
 */
bool SanitizeStepFile(
    const std::string& inputPath,
    const std::string& outputPath);