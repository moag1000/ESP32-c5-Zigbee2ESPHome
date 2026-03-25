#!/usr/bin/env node
/**
 * z2m_extract.js — Extract ALL device definitions from zigbee-herdsman-converters
 *
 * Supports v22–v26+ API:
 *   v22: require('zigbee-herdsman-converters/devices') export
 *   v26: models-index.json + per-file require() + prepareDefinition()
 *
 * Usage:
 *   npm install
 *   node z2m_extract.js [--output scripts/data/z2m_raw.json]
 */

'use strict';

const fs = require('fs');
const path = require('path');

// ---------------------------------------------------------------------------
// Import zigbee-herdsman-converters
// ---------------------------------------------------------------------------

let zhc;
let allDefinitions;

try {
    zhc = require('zigbee-herdsman-converters');
} catch (e) {
    console.error('Error: zigbee-herdsman-converters not installed.');
    console.error('Run: cd scripts && npm install');
    console.error('Detail:', e.message);
    process.exit(1);
}

// Strategy 1: v22-style — try require('zigbee-herdsman-converters/devices')
try {
    const devModule = require('zigbee-herdsman-converters/devices');
    allDefinitions = devModule.default || devModule;
    if (typeof allDefinitions === 'object' && Array.isArray(allDefinitions) === false) {
        allDefinitions = Object.values(allDefinitions).flat();
    }
    console.error('Loaded definitions via devices subpath (v22 style)');
} catch (e) {
    // Strategy 2: v26-style — models-index.json + per-file loading
    console.error('devices subpath unavailable, trying v26 models-index.json...');
    try {
        const basePath = path.dirname(require.resolve('zigbee-herdsman-converters'));
        const indexPath = path.join(basePath, 'models-index.json');
        const idx = JSON.parse(fs.readFileSync(indexPath, 'utf8'));

        // Collect unique device files from the index
        const fileSet = new Set();
        for (const model of Object.keys(idx)) {
            for (const [file] of idx[model]) {
                fileSet.add(file);
            }
        }

        console.error(`  Found ${Object.keys(idx).length} models across ${fileSet.size} device files`);

        allDefinitions = [];
        let fileErrors = 0;
        for (const file of fileSet) {
            const filePath = path.join(basePath, 'devices', file);
            try {
                const mod = require(filePath);
                let defs = mod.default || mod;
                if (typeof defs === 'object' && Array.isArray(defs) === false) {
                    defs = Object.values(defs).flat();
                }
                if (Array.isArray(defs)) {
                    allDefinitions = allDefinitions.concat(defs);
                }
            } catch (fileErr) {
                fileErrors++;
                if (fileErrors <= 5) {
                    console.error(`  Error loading ${file}: ${fileErr.message.substring(0, 100)}`);
                }
            }
        }
        if (fileErrors > 5) {
            console.error(`  ... and ${fileErrors - 5} more file errors`);
        }
        console.error(`  Loaded ${allDefinitions.length} raw definitions from ${fileSet.size - fileErrors} files`);
    } catch (e2) {
        console.error('Error: Could not load device definitions.');
        console.error('Detail:', e2.message);
        process.exit(1);
    }
}

// ---------------------------------------------------------------------------
// Build reverse map: converter object → name string
// ---------------------------------------------------------------------------

function buildConverterNameMap(converters, prefix) {
    const map = new Map();
    if (!converters) return map;
    const obj = converters.default || converters;
    for (const [name, val] of Object.entries(obj)) {
        if (val && typeof val === 'object') {
            map.set(val, `${prefix}.${name}`);
        }
    }
    return map;
}

const fzNameMap = buildConverterNameMap(zhc.fromZigbee || zhc.fromZigbeeConverters || zhc.fz, 'fz');
const tzNameMap = buildConverterNameMap(zhc.toZigbee || zhc.toZigbeeConverters || zhc.tz, 'tz');

// Also check for tuya-specific converters
try {
    const tuyaMod = require('zigbee-herdsman-converters/lib/tuya');
    const tuyaObj = tuyaMod.default || tuyaMod;
    if (tuyaObj.fz) {
        for (const [name, obj] of Object.entries(tuyaObj.fz)) {
            if (obj && typeof obj === 'object') {
                fzNameMap.set(obj, `tuya.fz.${name}`);
            }
        }
    }
    if (tuyaObj.tz) {
        for (const [name, obj] of Object.entries(tuyaObj.tz)) {
            if (obj && typeof obj === 'object') {
                tzNameMap.set(obj, `tuya.tz.${name}`);
            }
        }
    }
} catch (e) {
    // tuya module may not be directly importable in this version
}

console.error(`  fz name map: ${fzNameMap.size} entries`);
console.error(`  tz name map: ${tzNameMap.size} entries`);

// ---------------------------------------------------------------------------
// Expose serialization
// ---------------------------------------------------------------------------

function serializeExpose(expose) {
    if (!expose) return null;

    const result = {};

    // Type — v26 may use constructor name as type
    if (expose.type) {
        result.type = expose.type;
    } else if (expose.constructor && expose.constructor.name !== 'Object') {
        // v26 ModernExtend exposes use class instances
        const className = expose.constructor.name.toLowerCase();
        if (['light', 'switch', 'cover', 'lock', 'climate', 'fan'].includes(className)) {
            result.type = className;
        }
    }

    // Name and property
    if (expose.name) result.name = expose.name;
    if (expose.property) result.property = expose.property;

    // Label and description
    if (expose.label) result.label = expose.label;
    if (expose.description) result.description = expose.description;

    // Access flags
    if (expose.access !== undefined) result.access = expose.access;

    // Unit
    if (expose.unit) result.unit = expose.unit;

    // Numeric range
    if (expose.value_min !== undefined) result.value_min = expose.value_min;
    if (expose.value_max !== undefined) result.value_max = expose.value_max;
    if (expose.value_step !== undefined) result.value_step = expose.value_step;

    // Enum values
    if (expose.values && expose.values.length > 0) result.values = expose.values;

    // Binary on/off values
    if (expose.value_on !== undefined) result.value_on = expose.value_on;
    if (expose.value_off !== undefined) result.value_off = expose.value_off;

    // Endpoint
    if (expose.endpoint) result.endpoint = expose.endpoint;

    // Category (if set)
    if (expose.category) result.category = expose.category;

    // Features (sub-exposes for composite types like light, climate, etc.)
    if (expose.features && expose.features.length > 0) {
        result.features = expose.features.map(f => serializeExpose(f)).filter(Boolean);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Converter serialization
// ---------------------------------------------------------------------------

function serializeFz(fzArr) {
    if (!fzArr || Array.isArray(fzArr) === false) return [];
    return fzArr.map(fz => {
        const entry = {};
        // Get name from reverse map
        const name = fzNameMap.get(fz);
        if (name) entry.name = name;
        // Cluster
        if (fz.cluster) entry.cluster = fz.cluster;
        // Type
        if (fz.type) entry.type = fz.type;
        return entry;
    }).filter(e => e.name || e.cluster);
}

function serializeTz(tzArr) {
    if (!tzArr || Array.isArray(tzArr) === false) return [];
    return tzArr.map(tz => {
        const entry = {};
        // Get name from reverse map
        const name = tzNameMap.get(tz);
        if (name) entry.name = name;
        // Key
        if (tz.key) entry.key = tz.key;
        return entry;
    }).filter(e => e.name || (e.key && e.key.length > 0));
}

// ---------------------------------------------------------------------------
// Fingerprint extraction
// ---------------------------------------------------------------------------

function extractFingerprints(def) {
    const fps = [];

    // From fingerprint array
    if (def.fingerprint && Array.isArray(def.fingerprint)) {
        for (const fp of def.fingerprint) {
            if (fp.modelID || fp.manufacturerName) {
                fps.push({
                    modelID: fp.modelID || null,
                    manufacturerName: fp.manufacturerName || null,
                });
            }
        }
    }

    // From zigbeeModel array
    if (def.zigbeeModel && Array.isArray(def.zigbeeModel)) {
        for (const zm of def.zigbeeModel) {
            // Check if this model is already covered by a fingerprint
            const exists = fps.some(fp => fp.modelID === zm);
            if (!exists) {
                fps.push({ modelID: zm, manufacturerName: null });
            }
        }
    }

    return fps;
}

// ---------------------------------------------------------------------------
// Meta / Tuya DP extraction
// ---------------------------------------------------------------------------

function extractMeta(resolved) {
    // In v26, meta may be on the prepared definition, or from extend
    const meta = resolved.meta;
    if (!meta) return null;

    const result = {};

    // Tuya datapoints
    if (meta.tuyaDatapoints) {
        result.tuyaDatapoints = meta.tuyaDatapoints.map(dp => {
            if (Array.isArray(dp)) {
                const entry = { dp: dp[0] };
                if (typeof dp[1] === 'string') entry.name = dp[1];
                if (dp[2]) {
                    if (typeof dp[2] === 'object' && dp[2].from) {
                        entry.converter = 'custom';
                    } else if (typeof dp[2] === 'string') {
                        entry.converter = dp[2];
                    } else if (typeof dp[2] === 'object' && dp[2].name) {
                        entry.converter = dp[2].name;
                    }
                }
                return entry;
            }
            return dp;
        });
    }

    // Multi-endpoint
    if (meta.multiEndpoint) result.multiEndpoint = true;
    if (meta.multiEndpointSkip) result.multiEndpointSkip = meta.multiEndpointSkip;

    // Tuya quirks
    if (meta.tuyaSendCommand) result.tuyaSendCommand = meta.tuyaSendCommand;

    return Object.keys(result).length > 0 ? result : null;
}

// ---------------------------------------------------------------------------
// Main extraction
// ---------------------------------------------------------------------------

function extractDefinitions() {
    const definitions = allDefinitions || [];
    console.error(`Processing ${definitions.length} device definitions...`);

    const devices = [];
    let exposeErrors = 0;
    let preparedCount = 0;
    let processErrors = 0;

    for (const def of definitions) {
        try {
            // Always try prepareDefinition — v26 uses extend exclusively
            let resolved = def;
            if (zhc.prepareDefinition) {
                try {
                    resolved = zhc.prepareDefinition(def);
                    if (resolved) preparedCount++;
                } catch (e) {
                    resolved = def;
                }
            }

            // Resolve exposes (can be array or function)
            let exposes = [];
            if (typeof resolved.exposes === 'function') {
                try {
                    exposes = resolved.exposes({getEndpoint: () => null, endpoints: () => []}, {});
                } catch (e) {
                    try {
                        exposes = resolved.exposes();
                    } catch (e2) {
                        exposeErrors++;
                        exposes = [];
                    }
                }
            } else if (Array.isArray(resolved.exposes)) {
                exposes = resolved.exposes;
            }

            const device = {
                model: def.model || '',
                vendor: def.vendor || '',
                description: def.description || '',
                fingerprints: extractFingerprints(def),
                exposes: exposes.map(e => serializeExpose(e)).filter(Boolean),
                fromZigbee: serializeFz(resolved.fromZigbee || def.fromZigbee),
                toZigbee: serializeTz(resolved.toZigbee || def.toZigbee),
            };

            // Meta (Tuya DPs, multi-endpoint, etc.) — check resolved first, then original
            const meta = extractMeta(resolved) || extractMeta(def);
            if (meta) device.meta = meta;

            // Options
            const opts = resolved.options || def.options;
            if (opts && opts.length > 0) {
                device.options = opts.map(o => serializeExpose(o)).filter(Boolean);
            }

            devices.push(device);
        } catch (e) {
            processErrors++;
            if (processErrors <= 10) {
                console.error(`  Error processing ${def.model || 'unknown'}: ${e.message}`);
            }
        }
    }

    if (processErrors > 10) {
        console.error(`  ... and ${processErrors - 10} more processing errors`);
    }
    if (exposeErrors > 0) {
        console.error(`  ${exposeErrors} devices had expose resolution errors (fallback to empty)`);
    }
    console.error(`  ${preparedCount} devices resolved via prepareDefinition()`);
    console.error(`  ${processErrors} total processing errors`);

    return devices;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

const args = process.argv.slice(2);
let outputPath = path.join(__dirname, 'data', 'z2m_raw.json');

for (let i = 0; i < args.length; i++) {
    if (args[i] === '--output' && args[i + 1]) {
        outputPath = args[i + 1];
        i++;
    }
}

console.error('Extracting z2m device definitions...');
const devices = extractDefinitions();

// Write output
let zhcVersion = 'unknown';
try {
    const pkgPath = require.resolve('zigbee-herdsman-converters/package.json');
    zhcVersion = require(pkgPath).version || 'unknown';
} catch (e) {}

const output = {
    _meta: {
        source: 'zigbee-herdsman-converters',
        version: zhcVersion,
        extracted: new Date().toISOString(),
        count: devices.length,
    },
    devices: devices,
};

fs.mkdirSync(path.dirname(outputPath), { recursive: true });
fs.writeFileSync(outputPath, JSON.stringify(output, null, 0));

const stats = fs.statSync(outputPath);
console.error(`Written ${devices.length} devices to ${outputPath} (${(stats.size / 1024 / 1024).toFixed(1)} MB)`);

// Print summary stats
const withExposes = devices.filter(d => d.exposes.length > 0).length;
const withFz = devices.filter(d => d.fromZigbee.length > 0).length;
const withTz = devices.filter(d => d.toZigbee.length > 0).length;
const withFp = devices.filter(d => d.fingerprints.length > 0).length;
const withMeta = devices.filter(d => d.meta).length;

console.error(`\nSummary:`);
console.error(`  Total devices:    ${devices.length}`);
console.error(`  With exposes:     ${withExposes}`);
console.error(`  With fz:          ${withFz}`);
console.error(`  With tz:          ${withTz}`);
console.error(`  With fingerprint: ${withFp}`);
console.error(`  With meta/tuya:   ${withMeta}`);
