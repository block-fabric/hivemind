<script lang="ts">
  // The chance of each state of a market over time: a line per state, from even odds when the market
  // opened to the price now, at the blocks this app saw the prices move (the chain keeps no history of
  // them). The state chosen is drawn thicker; hovering shows the chances at a block.
  import { formatChance } from './model';
  import type { PricePoint } from './store.svelte';

  let {
    points,
    now,
    height,
    chosen = -1,
    tall = 220,
    compact = false,
    label,
  }: {
    /** As the journal keeps them: prices in ten-thousandths. */
    points: readonly PricePoint[];
    /** The prices now, and the height they are of. */
    now: readonly number[];
    height: number;
    chosen?: number;
    tall?: number;
    compact?: boolean;
    label: string;
  } = $props();

  let width = $state(480);
  const pad = $derived(compact ? { l: 2, r: 2, t: 4, b: 4 } : { l: 38, r: 10, t: 10, b: 22 });

  /** The points, with now at the end. */
  const series = $derived.by(() => {
    const list = points.map((p) => ({ h: p.h, p: p.p.map((x) => x / 10000) }));
    const last = list[list.length - 1];
    if (now.length && (!last || last.h < height || last.p.some((x, i) => Math.abs(x - (now[i] ?? 0)) > 1e-4))) list.push({ h: Math.max(height, last?.h ?? height), p: now.slice() });
    if (list.length === 1) list.unshift({ h: list[0].h - 1, p: list[0].p.map(() => 1 / list[0].p.length) });
    return list.filter((x) => x.p.length === now.length);
  });

  const x0 = $derived(series.length ? series[0].h : 0);
  const x1 = $derived(series.length ? Math.max(series[series.length - 1].h, x0 + 1) : 1);
  const xOf = (h: number) => pad.l + ((h - x0) / (x1 - x0)) * Math.max(1, width - pad.l - pad.r);
  const yOf = (p: number) => pad.t + (1 - p) * (tall - pad.t - pad.b);

  /** A step line per state: a price holds until the next block it was seen to move at. */
  const paths = $derived(
    now.map((_, s) =>
      series
        .map((pt, i) => {
          const x = xOf(pt.h).toFixed(1);
          const y = yOf(pt.p[s]).toFixed(1);
          if (i === 0) return `M${x} ${y}`;
          return `H${x}V${y}`;
        })
        .join(''),
    ),
  );

  let hover = $state<number | null>(null);
  function move(e: PointerEvent) {
    const svg = e.currentTarget as SVGSVGElement;
    const r = svg.getBoundingClientRect();
    const x = ((e.clientX - r.left) / Math.max(1, r.width)) * width;
    let best = 0;
    for (let i = 0; i < series.length; i++) if (Math.abs(xOf(series[i].h) - x) < Math.abs(xOf(series[best].h) - x)) best = i;
    hover = series.length ? best : null;
  }
  const hovered = $derived(hover === null ? null : series[hover]);
</script>

<div class="chart" class:compact bind:clientWidth={width}>
  <svg width={width} height={tall} viewBox="0 0 {width} {tall}" role="img" aria-label={label} onpointermove={compact ? undefined : move} onpointerleave={() => (hover = null)}>
    {#if !compact}
      {#each [0, 0.25, 0.5, 0.75, 1] as g (g)}
        <line class="grid" x1={pad.l} x2={width - pad.r} y1={yOf(g)} y2={yOf(g)} />
        <text class="axis" x={pad.l - 6} y={yOf(g) + 4} text-anchor="end">{Math.round(g * 100)}%</text>
      {/each}
      {#if series.length}
        <text class="axis" x={pad.l} y={tall - 5}>{series[0].h}</text>
        <text class="axis" x={width - pad.r} y={tall - 5} text-anchor="end">{series[series.length - 1].h}</text>
      {/if}
    {/if}
    {#each paths as d, s (s)}
      <path class="line hm-c{s % 8}" class:chosen={s === chosen} class:faded={chosen >= 0 && s !== chosen} {d} />
    {/each}
    {#if hovered}
      <line class="cursor" x1={xOf(hovered.h)} x2={xOf(hovered.h)} y1={pad.t} y2={tall - pad.b} />
      {#each hovered.p as p, s (s)}<circle class="hm-c{s % 8} pt" cx={xOf(hovered.h)} cy={yOf(p)} r="3.5" />{/each}
    {/if}
  </svg>
  {#if hovered}
    <div class="tip" role="status">
      <span class="faint">Block {hovered.h}</span>
      {#each hovered.p as p, s (s)}<span class="row"><span class="hm-dot hm-c{s % 8}"></span><span class="num">{formatChance(p)}</span></span>{/each}
    </div>
  {/if}
</div>

<style>
  .chart {
    position: relative;
    width: 100%;
    min-width: 0;
  }
  svg {
    display: block;
    width: 100%;
    touch-action: none;
  }
  .grid {
    stroke: var(--line);
    stroke-width: 1;
  }
  .axis {
    fill: var(--text-muted);
    font-size: 11px;
    font-family: var(--font-num);
  }
  .line {
    fill: none;
    stroke: var(--hm-c);
    stroke-width: 2;
    stroke-linejoin: round;
  }
  .compact .line {
    stroke-width: 1.6;
  }
  .line.chosen {
    stroke-width: 3;
  }
  .line.faded {
    opacity: 0.45;
  }
  .cursor {
    stroke: var(--line-strong);
    stroke-dasharray: 3 3;
  }
  .pt {
    fill: var(--hm-c);
    stroke: var(--surface);
    stroke-width: 1.5;
  }
  .tip {
    position: absolute;
    top: 0.3rem;
    right: 0.4rem;
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem 0.7rem;
    max-width: 70%;
    padding: 0.3rem 0.6rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-s);
    background: var(--surface);
    box-shadow: var(--shadow);
    font-size: 0.8rem;
  }
  .tip .row {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
  }
</style>
