/** Declarative metadata for a WASM indicator plugin. */
export type WasmPlotKind = "line" | "histogram" | "area" | "band" | "markers";

export interface WasmParamSpec {
  id: string;
  label: string;
  type: "int" | "float";
  default: number;
  min?: number;
  max?: number;
}

export interface WasmOutputSpec {
  id: string;
  title: string;
  pane: string;
  plot: WasmPlotKind;
  color: string;
  lineWidth?: number;
  channels?: number;
}

export interface WasmManifest {
  schema: 1;
  abi: 1;
  id: string;
  name: string;
  version: string;
  wasm: string;
  params: WasmParamSpec[];
  outputs: WasmOutputSpec[];
}

/** A computed output aligned to the input bars. */
export interface WasmSeries {
  id: string;
  title: string;
  pane: string;
  plot: WasmPlotKind;
  color: string;
  lineWidth: number;
  values: Float64Array;
  lower?: Float64Array;
}

/** UI/runtime state for one loaded or loading plugin. */
export interface WasmPluginView {
  id: string;
  name: string;
  version: string;
  enabled: boolean;
  status: "loading" | "ready" | "error";
  params: WasmParamSpec[];
  values: number[];
  error?: string;
}

const ID_RE = /^[a-z0-9][a-z0-9._-]{0,63}$/i;
const COLOR_RE = /^#[0-9a-f]{6}$/i;
const PLOTS = new Set<WasmPlotKind>(["line", "histogram", "area", "band", "markers"]);
const TITLE_PARAM_RE = /\{([a-z0-9._-]+)\}/gi;

function fail(message: string): never {
  throw new Error(`invalid WASM indicator manifest: ${message}`);
}

function stringField(value: unknown, field: string, max = 96): string {
  if (typeof value !== "string" || value.trim().length === 0 || value.length > max) {
    fail(`${field} must be a non-empty string`);
  }
  return value.trim();
}

function finiteNumber(value: unknown, field: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    fail(`${field} must be a finite number`);
  }
  return value;
}

function parseParam(value: unknown, index: number): WasmParamSpec {
  if (!value || typeof value !== "object") fail(`params[${index}] must be an object`);
  const row = value as Record<string, unknown>;
  const id = stringField(row.id, `params[${index}].id`, 64);
  if (!ID_RE.test(id)) fail(`params[${index}].id is invalid`);
  const type = row.type === "int" || row.type === "float" ? row.type : fail(
    `params[${index}].type must be int or float`,
  );
  const defaultValue = finiteNumber(row.default, `params[${index}].default`);
  const min = row.min === undefined ? undefined : finiteNumber(row.min, `params[${index}].min`);
  const max = row.max === undefined ? undefined : finiteNumber(row.max, `params[${index}].max`);
  if (min !== undefined && max !== undefined && min > max) {
    fail(`params[${index}].min must not exceed max`);
  }
  if (defaultValue < (min ?? Number.NEGATIVE_INFINITY) || defaultValue > (max ?? Number.POSITIVE_INFINITY)) {
    fail(`params[${index}].default is outside min/max`);
  }
  const label = typeof row.label === "string" && row.label.trim() ? row.label.trim() : id;
  return { id, label, type, default: defaultValue, ...(min === undefined ? {} : { min }), ...(max === undefined ? {} : { max }) };
}

function parseOutput(value: unknown, index: number): WasmOutputSpec {
  if (!value || typeof value !== "object") fail(`outputs[${index}] must be an object`);
  const row = value as Record<string, unknown>;
  const id = stringField(row.id, `outputs[${index}].id`, 64);
  if (!ID_RE.test(id)) fail(`outputs[${index}].id is invalid`);
  const plot = typeof row.plot === "string" && PLOTS.has(row.plot as WasmPlotKind)
    ? (row.plot as WasmPlotKind)
    : fail(`outputs[${index}].plot is unsupported`);
  const pane = typeof row.pane === "string" && row.pane.trim() ? row.pane.trim() : "main";
  if (pane.length > 64 || !/^[a-z0-9:_-]+$/i.test(pane)) {
    fail(`outputs[${index}].pane is invalid`);
  }
  const title = typeof row.title === "string" && row.title.trim() ? row.title.trim() : id;
  const color = typeof row.color === "string" && COLOR_RE.test(row.color) ? row.color : "#53b7ff";
  const width = row.lineWidth === undefined ? 1 : finiteNumber(row.lineWidth, `outputs[${index}].lineWidth`);
  const channels = row.channels === undefined
    ? (plot === "band" ? 2 : 1)
    : finiteNumber(row.channels, `outputs[${index}].channels`);
  if (channels !== 1 && channels !== 2) fail(`outputs[${index}].channels must be 1 or 2`);
  return {
    id,
    title,
    pane,
    plot,
    color,
    lineWidth: Math.max(0.5, Math.min(4, width)),
    channels,
  };
}

/** Parse a manifest and reject anything the host cannot safely render. */
export function parseManifest(value: unknown): WasmManifest {
  if (!value || typeof value !== "object") fail("manifest must be an object");
  const row = value as Record<string, unknown>;
  if (row.schema !== 1 || row.abi !== 1) fail("schema/abi must be 1");
  const id = stringField(row.id, "id", 64);
  if (!ID_RE.test(id)) fail("id is invalid");
  const name = stringField(row.name, "name");
  const version = stringField(row.version, "version", 32);
  const wasm = stringField(row.wasm, "wasm", 256);
  if (!/^[a-zA-Z0-9._-]+\.wasm$/.test(wasm) || wasm.includes("..")) {
    fail("wasm must be a simple relative .wasm filename");
  }
  if (!Array.isArray(row.params) || row.params.length > 8) {
    fail("params must be an array of at most 8 entries");
  }
  if (!Array.isArray(row.outputs) || row.outputs.length === 0 || row.outputs.length > 16) {
    fail("outputs must be an array of 1..16 entries");
  }
  const params = row.params.map(parseParam);
  const outputs = row.outputs.map(parseOutput);
  if (new Set(params.map((item) => item.id)).size !== params.length) fail("duplicate param id");
  const paramIds = new Set(params.map((item) => item.id));
  for (const output of outputs) {
    for (const match of output.title.matchAll(TITLE_PARAM_RE)) {
      if (!paramIds.has(match[1])) fail(`unknown title parameter ${match[1]}`);
    }
  }
  if (new Set(outputs.map((item) => item.id)).size !== outputs.length) fail("duplicate output id");
  if (outputs.reduce((sum, output) => sum + (output.channels ?? 1), 0) > 16) {
    fail("too many output channels");
  }
  return { schema: 1, abi: 1, id, name, version, wasm, params, outputs };
}

export function outputChannelCount(manifest: WasmManifest): number {
  return manifest.outputs.reduce((sum, output) => sum + (output.channels ?? 1), 0);
}

function formatParam(value: number): string {
  if (Number.isInteger(value)) return String(value);
  return String(Number(value.toFixed(4)));
}

export function resolveOutputTitle(
  title: string,
  manifest: WasmManifest,
  values: number[],
): string {
  return title.replace(TITLE_PARAM_RE, (_, id: string) => {
    const index = manifest.params.findIndex((param) => param.id === id);
    if (index < 0) return `{${id}}`;
    return formatParam(values[index] ?? manifest.params[index].default);
  });
}

export function seriesFromManifest(
  manifest: WasmManifest,
  arrays: Float64Array[],
  params: number[] = manifest.params.map((param) => param.default),
): WasmSeries[] {
  const expected = outputChannelCount(manifest);
  if (arrays.length !== expected) {
    throw new Error(`plugin returned ${arrays.length} channels, expected ${expected}`);
  }
  let cursor = 0;
  return manifest.outputs.map((output) => {
    const values = arrays[cursor++];
    const lower = (output.channels ?? 1) > 1 ? arrays[cursor++] : undefined;
    return {
      id: output.id,
      title: resolveOutputTitle(output.title, manifest, params),
      pane: output.pane,
      plot: output.plot,
      color: output.color,
      lineWidth: output.lineWidth ?? 1,
      values,
      ...(lower === undefined ? {} : { lower }),
    };
  });
}
