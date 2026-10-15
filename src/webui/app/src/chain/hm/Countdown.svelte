<script lang="ts">
  // How long until a deadline of the markets (the end of a period, its reveal phase): in time, or in
  // blocks on a test chain whose periods are counted in blocks. Ticks every second.
  import { t } from '../../lib/i18n.svelte';
  import { node } from '../../lib/state.svelte';
  import { untilIn, type HivemindInfo } from './model';
  import { every } from '../../lib/poll';

  /**
   * phrase: the words around the time, so that a deadline that came reads as a sentence ("ends now", not "ends in
   * now": audit port-1 #10). Without one, the time alone ("now" when it came).
   */
  let { info, target, urgent = 0, phrase }: { info: HivemindInfo; target: number; urgent?: number; phrase?: 'ends' | 'start' | 'starts' | 'in' | 'closes' } = $props();

  let now = $state(Math.floor(Date.now() / 1000));
  $effect(() => every(1000, () => (now = Math.floor(Date.now() / 1000))));
  const height = $derived(node.tip?.height ?? 0);
  const left = $derived(info.periodblocks ? target - (height + 1) : target - now);
  const words = $derived(untilIn(info, target, now, height));
  const shown = $derived.by(() => {
    const done = left <= 0;
    switch (phrase) {
      case 'ends':
        return done ? t('ends now') : t('ends in %1', words);
      case 'start':
        return done ? t('start now') : t('start in %1', words);
      case 'starts':
        return done ? t('starts now') : t('starts in %1', words);
      case 'closes':
        return done ? t('closes now') : t('closes in %1', words);
      case 'in':
        return done ? t('now') : t('in %1', words);
      default:
        return done ? t('now') : words;
    }
  });
</script>

<span class="countdown num" class:urgent={urgent > 0 && left <= urgent} class:over={left <= 0} title={info.periodblocks ? 'Periods are counted in blocks of this chain' : 'Counted in the time of the mainchain'}>{shown}</span>

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
