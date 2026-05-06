#!/usr/bin/env node
/**
 * CLI wrapper for Khronos gltf-validator: validates standalone .glb files produced by Model2Tile.
 */
import { createRequire } from 'module';
import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

const require = createRequire(import.meta.url);
const validator = require('gltf-validator');

const kSeverityError = 0;
const kSeverityWarning = 1;

function printHelp() {
  console.error(`Usage: node validate_glb.mjs [options] <path...>

Validate glTF 2.0 binary (.glb) files using Khronos gltf-validator.
Paths may be files or directories (directories are scanned recursively; node_modules skipped).

Options:
  --fail-on-warnings   Exit with status 1 if any warning or error is reported
  --json               Print validation reports as JSON (see below)
  -h, --help           Show this help

Exit status:
  0  No errors (and no warnings if --fail-on-warnings)
  1  Validation failures, missing paths, or runtime errors

With --json, prints a JSON array of objects: { "file": "<path>", "report": <validator report> }.
Human-readable issue lines go to stderr unless --json is the only output desired (errors still on stderr).
`);
}

async function collectGlbFiles(entryPath) {
  const resolved = path.resolve(entryPath);
  let stat;
  try {
    stat = await fs.stat(resolved);
  } catch {
    return { error: `not found: ${entryPath}` };
  }

  if (stat.isFile()) {
    if (!resolved.toLowerCase().endsWith('.glb')) {
      return { error: `not a .glb file: ${entryPath}` };
    }
    return { files: [resolved] };
  }

  if (!stat.isDirectory()) {
    return { error: `not a file or directory: ${entryPath}` };
  }

  const files = [];

  async function walk(dir) {
    const entries = await fs.readdir(dir, { withFileTypes: true });
    for (const ent of entries) {
      if (ent.name === 'node_modules' || ent.name.startsWith('.')) {
        continue;
      }
      const full = path.join(dir, ent.name);
      if (ent.isDirectory()) {
        await walk(full);
      } else if (ent.isFile() && ent.name.toLowerCase().endsWith('.glb')) {
        files.push(full);
      }
    }
  }

  await walk(resolved);
  return { files };
}

function summarizeIssues(report) {
  const issues = report?.issues;
  if (!issues) {
    return { errors: 0, warnings: 0, infos: 0, hints: 0 };
  }
  return {
    errors: issues.numErrors ?? 0,
    warnings: issues.numWarnings ?? 0,
    infos: issues.numInfos ?? 0,
    hints: issues.numHints ?? 0,
  };
}

async function validateOneFile(filePath, jsonMode) {
  const buf = await fs.readFile(filePath);
  const uri = path.basename(filePath);

  const report = await validator.validateBytes(new Uint8Array(buf), {
    uri,
    format: 'glb',
    maxIssues: 0,
    writeTimestamp: true,
  });

  const counts = summarizeIssues(report);

  if (!jsonMode) {
    const rel = filePath;
    if (
      counts.errors === 0 &&
      counts.warnings === 0 &&
      counts.infos === 0 &&
      counts.hints === 0
    ) {
      console.error(`${rel}: OK`);
    } else {
      console.error(
        `${rel}: errors=${counts.errors} warnings=${counts.warnings} infos=${counts.infos} hints=${counts.hints}`
      );
      const messages = report?.issues?.messages;
      if (Array.isArray(messages)) {
        for (const m of messages) {
          const sev =
            m.severity === kSeverityError
              ? 'ERROR'
              : m.severity === kSeverityWarning
                ? 'WARN'
                : m.severity === 2
                  ? 'INFO'
                  : 'HINT';
          const loc = m.pointer ?? (m.offset !== undefined ? `GLB@${m.offset}` : '');
          console.error(`  [${sev}] ${m.code} ${loc}: ${m.message}`);
        }
      }
    }
  }

  return { filePath, report, counts };
}

async function main() {
  const args = process.argv.slice(2);
  let failOnWarnings = false;
  let jsonMode = false;
  const paths = [];

  for (let i = 0; i < args.length; ++i) {
    const a = args[i];
    if (a === '--fail-on-warnings') {
      failOnWarnings = true;
    } else if (a === '--json') {
      jsonMode = true;
    } else if (a === '-h' || a === '--help') {
      printHelp();
      process.exit(0);
    } else if (a.startsWith('-')) {
      console.error(`Unknown option: ${a}`);
      printHelp();
      process.exit(1);
    } else {
      paths.push(a);
    }
  }

  if (paths.length === 0) {
    printHelp();
    process.exit(1);
  }

  const allFiles = [];
  for (const p of paths) {
    const result = await collectGlbFiles(p);
    if (result.error) {
      console.error(`validate_glb: ${result.error}`);
      process.exit(1);
    }
    allFiles.push(...result.files);
  }

  if (allFiles.length === 0) {
    console.error('validate_glb: no .glb files found');
    process.exit(1);
  }

  allFiles.sort();

  const jsonOut = [];
  let hadRuntimeError = false;
  let maxErrors = 0;
  let maxWarnings = 0;

  for (const filePath of allFiles) {
    try {
      const { report, counts } = await validateOneFile(filePath, jsonMode);
      maxErrors = Math.max(maxErrors, counts.errors);
      maxWarnings = Math.max(maxWarnings, counts.warnings);
      if (jsonMode) {
        jsonOut.push({ file: filePath, report });
      }
    } catch (e) {
      hadRuntimeError = true;
      console.error(`validate_glb: ${filePath}: ${e?.message ?? e}`);
    }
  }

  if (jsonMode) {
    console.log(JSON.stringify(jsonOut, null, 2));
  }

  if (hadRuntimeError) {
    process.exit(1);
  }

  const fail =
    maxErrors > 0 || (failOnWarnings && maxWarnings > 0);
  process.exit(fail ? 1 : 0);
}

main().catch((e) => {
  console.error(`validate_glb: ${e?.message ?? e}`);
  process.exit(1);
});
