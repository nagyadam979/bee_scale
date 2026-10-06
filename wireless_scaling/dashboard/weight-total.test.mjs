import assert from "node:assert/strict";
import test from "node:test";

import {
  densifyTotalSeries,
  estimateSamplePeriod,
  totalKgSeries,
} from "./public/weight-total.js";

const MINUTE = 60 * 1000;
const point = (minute, kg) => [minute * MINUTE, kg];

test("keeps totals while four 20-minute boards drift beyond eight minutes", () => {
  const perBoard = [
    [point(0, 1), point(20, 2), point(40, 3), point(60, 4)],
    [point(8.3, 10), point(28.8, 20), point(49.3, 30), point(69.9, 40)],
    [point(4, 100), point(24, 200), point(44, 300), point(64, 400)],
    [point(6, 1000), point(26, 2000), point(46, 3000), point(66, 4000)],
  ];

  assert.deepEqual(totalKgSeries(perBoard).map((p) => p[1]), [1111, 2222, 3333, 4444]);
});

test("does not synthesize a total when one board is missing", () => {
  const perBoard = [
    [point(0, 1), point(20, 2), point(40, 3)],
    [point(5, 10), point(25, 20), point(45, 30)],
    [point(7, 100), point(47, 300)],
    [point(9, 1000), point(29, 2000), point(49, 3000)],
  ];
  const totals = totalKgSeries(perBoard);

  assert.deepEqual(totals.map((p) => p[1]), [1111, 3333]);
  const plot = densifyTotalSeries(totals, 5 * MINUTE, 6, estimateSamplePeriod(perBoard) * 1.8);
  assert.equal(plot.filter((p) => p[1] === null).length, 1);
});

test("sums the actual short load-transition samples without board interpolation", () => {
  const perBoard = [
    [point(0, 10), point(20, 50), point(40, 10)],
    [point(3, 20), point(23, 60), point(43, 20)],
    [point(6, 30), point(26, 70), point(46, 30)],
    [point(9, 40), point(29, 80), point(49, 40)],
  ];

  assert.deepEqual(totalKgSeries(perBoard).map((p) => p[1]), [100, 260, 100]);
});

test("assigns a half-cycle sample once and resolves the tie toward the earlier cycle", () => {
  const perBoard = [
    [point(0, 1), point(20, 2)],
    [point(10, 10)],
  ];

  assert.deepEqual(totalKgSeries(perBoard).map((p) => p[1]), [11]);
});

test("pairs a two-board five-minute installation using its estimated period", () => {
  const perBoard = [
    [point(0, 1), point(5, 2), point(10, 3)],
    [point(2, 10), point(7, 20), point(12, 30)],
  ];

  assert.equal(estimateSamplePeriod(perBoard), 5 * MINUTE);
  assert.deepEqual(totalKgSeries(perBoard).map((p) => p[1]), [11, 22, 33]);
});

test("switches to a partial sum after a board has been silent beyond the retire limit", () => {
  const perBoard = [
    [point(0, 1), point(20, 1), point(40, 1), point(60, 1), point(80, 1), point(100, 1)],
    [point(2, 10), point(22, 10), point(42, 10), point(62, 10), point(82, 10), point(102, 10)],
    [point(4, 100), point(24, 100), point(44, 100)],
  ];
  const totals = totalKgSeries(perBoard, { retireAfterMs: 30 * MINUTE });

  assert.deepEqual(totals.map((p) => [p[1], p[2]]), [[111, 3], [111, 3], [111, 3], [11, 2], [11, 2], [11, 2]]);
  const plot = densifyTotalSeries(totals, 5 * MINUTE, 6, estimateSamplePeriod(perBoard) * 1.8);
  assert.equal(plot.filter((p) => p[1] === null).length, 1, "the level jump is not interpolated");
});

test("keeps the all-board rule while the silence is shorter than the retire limit", () => {
  const perBoard = [
    [point(0, 1), point(20, 1), point(40, 1), point(60, 1)],
    [point(2, 10), point(22, 10), point(42, 10), point(62, 10)],
    [point(4, 100), point(24, 100), point(44, 100)],
  ];

  assert.deepEqual(totalKgSeries(perBoard, { retireAfterMs: 30 * MINUTE }).map((p) => p[1]), [111, 111, 111]);
});

test("ignores a board without any data only when retirement is enabled", () => {
  const perBoard = [[point(0, 1), point(20, 2)], [point(3, 10), point(23, 20)], []];

  assert.deepEqual(totalKgSeries(perBoard), []);
  assert.deepEqual(totalKgSeries(perBoard, { retireAfterMs: 30 * MINUTE }).map((p) => [p[1], p[2]]), [[11, 2], [22, 2]]);
});
