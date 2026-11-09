export type Theme = "dark" | "light";

export interface ChartColors {
  background: string;
  text: string;
  grid: string;
  border: string;
  crosshair: string;
  crosshairLabel: string;
  up: string;
  down: string;
  volumeUp: string;
  volumeDown: string;
  average: string;
  areaTop: string;
  areaBottom: string;
  ma: string[];
  bollUpper: string;
  bollMiddle: string;
  bollLower: string;
  macdDif: string;
  macdDea: string;
  positive: string;
  negative: string;
}

const DARK: ChartColors = {
  background: "#080d13",
  text: "#9aa7b8",
  grid: "#121c27",
  border: "#1d2b3a",
  crosshair: "#52657a",
  crosshairLabel: "#223043",
  up: "#16c784",
  down: "#ea3943",
  volumeUp: "rgba(22, 199, 132, 0.25)",
  volumeDown: "rgba(234, 57, 67, 0.25)",
  average: "#f0c75e",
  areaTop: "rgba(83, 183, 255, 0.25)",
  areaBottom: "rgba(83, 183, 255, 0.02)",
  ma: ["#53b7ff", "#f0c75e", "#c792ea", "#43d17c", "#ffb86b"],
  bollUpper: "#53b7ff",
  bollMiddle: "#f0c75e",
  bollLower: "#c792ea",
  macdDif: "#53b7ff",
  macdDea: "#f0c75e",
  positive: "#e04a4a",
  negative: "#23b39a",
};

const LIGHT: ChartColors = {
  background: "#fbfcfe",
  text: "#4d6077",
  grid: "#e9eef5",
  border: "#d5dfeb",
  crosshair: "#7e91a8",
  crosshairLabel: "#dce6f2",
  up: "#0a9e6e",
  down: "#d92d3a",
  volumeUp: "rgba(10, 158, 110, 0.2)",
  volumeDown: "rgba(217, 45, 58, 0.2)",
  average: "#b17d1e",
  areaTop: "rgba(35, 112, 190, 0.2)",
  areaBottom: "rgba(35, 112, 190, 0.01)",
  ma: ["#2c75c9", "#b17d1e", "#8751b8", "#248d5a", "#c17724"],
  bollUpper: "#2c75c9",
  bollMiddle: "#b17d1e",
  bollLower: "#8751b8",
  macdDif: "#2c75c9",
  macdDea: "#b17d1e",
  positive: "#c94b4b",
  negative: "#249c86",
};

export function chartColors(theme: Theme): ChartColors {
  return theme === "light" ? LIGHT : DARK;
}
