<script lang="ts">
  // How long until a deadline of the markets (the end of a period, its reveal phase): in time, or in
  // blocks on a test chain whose periods are counted in blocks. Ticks every second.
  import { node } from '../../lib/state.svelte';
  import { untilIn, type HivemindInfo } from './model';

  let { info, target, urgent = 0 }: { info: HivemindInfo; target: number; urgent?: number } = $props();

  let now = $state(Math.floor(Date.now() / 1000));
  $effect(() => {
    const timer = setInterval(() => (now = Math.floor(Date.now() / 1000)), 1000);
    return () => clearInterval(timer);
  });
  const height = $derived(node.tip?.height ?? 0);
  const left = $derived(info.periodblocks ? target - (height + 1) : target - now);
  const words = $derived(untilIn(info, target, now, height));
</script>

<span class="countdown num" class:urgent={urgent > 0 && left <= urgent} class:over={left <= 0} title={info.periodblocks ? 'Periods are counted in blocks of this chain' : 'Counted in the time of the mainchain'}>{left <= 0 ? 'now' : words}</span>

<style>
  .countdown {
    font-weight: 600;
    white-space: nowrap;
  }
  .urgent {
    color: var(--warning);
  }
  .over {
    color: var(--text-muted);
  }
</style>
