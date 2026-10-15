<script lang="ts">
  // The name of a state of a market: Yes or No (Low or High for a scale), or the options of a market on
  // several decisions answered yes (text of the chain, shown as it is).
  import SafeText from '../../components/SafeText.svelte';
  import type { Outcome } from './model';

  let { outcome }: { outcome: Outcome } = $props();
</script>

<span class="outcome">
  {#if outcome.kind === 'yes'}Yes{:else if outcome.kind === 'no'}No{:else if outcome.kind === 'high'}High{:else if outcome.kind === 'low'}Low{:else if outcome.kind === 'none'}None of them{:else}{#each outcome.options as o, i (i)}{#if i > 0}<span class="plus"> + </span>{/if}<SafeText text={o} />{/each}{/if}
</span>

<style>
  .outcome {
    min-width: 0;
    overflow-wrap: anywhere;
  }
  .plus {
    color: var(--text-faint);
  }
</style>
