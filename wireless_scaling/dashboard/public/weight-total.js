// Pure helpers for turning asynchronous, periodic board measurements into totals.
// A board sample is assigned to exactly one nearest anchor cycle; totals contain
// measured samples only and are emitted only when every board is represented.

function median(values) {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const mid = sorted.length >> 1;
  return sorted.length & 1
    ? sorted[mid]
    : (sorted[mid - 1] + sorted[mid]) / 2;
}

function sortedSeries(series) {
  return series
    .filter((point) => Array.isArray(point) && Number.isFinite(point[0]) && Number.isFinite(point[1]))
    .map((point) => [point[0], point[1]])
    .sort((a, b) => a[0] - b[0]);
}

function seriesPeriod(series) {
  const diffs = [];
  for (let i = 1; i < series.length; i++) {
    const diff = series[i][0] - series[i - 1][0];
    if (diff > 0) diffs.push(diff);
  }
  return median(diffs);
}

export function estimateSamplePeriod(perBoard) {
  return median(perBoard.map((series) => seriesPeriod(sortedSeries(series))).filter((v) => v > 0));
}

function nearestAnchorIndex(anchor, timestamp) {
  let lo = 0, hi = anchor.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (anchor[mid][0] < timestamp) lo = mid + 1;
    else hi = mid;
  }
  if (lo === 0) return 0;
  if (lo === anchor.length) return anchor.length - 1;
  const beforeDistance = timestamp - anchor[lo - 1][0];
  const afterDistance = anchor[lo][0] - timestamp;
  // An exact half-cycle tie belongs to the earlier cycle, deterministically.
  return beforeDistance <= afterDistance ? lo - 1 : lo;
}

export function totalKgSeries(perBoard) {
  if (!perBoard.length) return [];
  const boards = perBoard.map(sortedSeries);
  if (boards.some((series) => !series.length)) return [];

  const periodMs = estimateSamplePeriod(boards);
  if (!(periodMs > 0)) return [];

  // Use the board whose sample count is closest to the fleet median as anchor.
  // That avoids both a board with a long outage and one with duplicate uploads;
  // ties follow board order for deterministic half-cycle behaviour.
  const medianLength = median(boards.map((series) => series.length));
  let anchorIndex = 0;
  for (let i = 1; i < boards.length; i++) {
    if (Math.abs(boards[i].length - medianLength) < Math.abs(boards[anchorIndex].length - medianLength)) {
      anchorIndex = i;
    }
  }
  const anchor = boards[anchorIndex];
  const buckets = anchor.map((point) => {
    const selected = new Array(boards.length).fill(null);
    selected[anchorIndex] = point;
    return selected;
  });

  // Slightly above half a nominal period tolerates clock jitter while keeping
  // samples from a genuinely missing cycle out of a distant neighbouring one.
  const matchLimitMs = periodMs * 0.55;
  boards.forEach((series, boardIndex) => {
    if (boardIndex === anchorIndex) return;
    for (const point of series) {
      const bucketIndex = nearestAnchorIndex(anchor, point[0]);
      const distance = Math.abs(point[0] - anchor[bucketIndex][0]);
      if (distance > matchLimitMs) continue;
      const previous = buckets[bucketIndex][boardIndex];
      const previousDistance = previous
        ? Math.abs(previous[0] - anchor[bucketIndex][0])
        : Infinity;
      if (distance < previousDistance || (distance === previousDistance && point[0] < previous[0])) {
        buckets[bucketIndex][boardIndex] = point;
      }
    }
  });

  const totals = [];
  for (const bucket of buckets) {
    if (bucket.some((point) => point === null)) continue;
    const timestamp = Math.round(bucket.reduce((sum, point) => sum + point[0], 0) / bucket.length);
    const kg = bucket.reduce((sum, point) => sum + point[1], 0);
    totals.push([timestamp, +kg.toFixed(3)]);
  }
  return totals;
}

export function densifyTotalSeries(data, stepMs, maxPerGap, maxContinuousGapMs) {
  if (data.length < 2) return data;
  const out = [];
  for (let i = 0; i < data.length - 1; i++) {
    const a = data[i], b = data[i + 1];
    out.push(a);
    const gap = b[0] - a[0];
    if (gap > maxContinuousGapMs) {
      out.push([Math.round((a[0] + b[0]) / 2), null]);
      continue;
    }
    const count = Math.min(maxPerGap, Math.floor(gap / stepMs) - 1);
    for (let k = 1; k <= count; k++) {
      const timestamp = a[0] + k * stepMs;
      const kg = a[1] + (b[1] - a[1]) * (timestamp - a[0]) / gap;
      out.push([timestamp, +kg.toFixed(3)]);
    }
  }
  out.push(data[data.length - 1]);
  return out;
}
