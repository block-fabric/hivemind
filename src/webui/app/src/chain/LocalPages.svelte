<script lang="ts">
  // The pages of Hivemind (chain/local.ts lists them): the one shown.
  import Create from './pages/Create.svelte';
  import Decisions from './pages/Decisions.svelte';
  import Markets from './pages/Markets.svelte';
  import Payouts from './pages/Payouts.svelte';
  import Portfolio from './pages/Portfolio.svelte';
  import Votecoin from './pages/Votecoin.svelte';
  import Voting from './pages/Voting.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'markets'}<Markets />
{:else if page === 'portfolio'}<Portfolio />
{:else if page === 'voting'}<Voting />
{:else if page === 'decisions'}<Decisions />
{:else if page === 'create'}<Create />
{:else if page === 'votecoin'}<Votecoin />
{:else if page === 'payouts'}<Payouts />
{/if}
