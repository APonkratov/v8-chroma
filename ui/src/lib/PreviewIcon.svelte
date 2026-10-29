<script lang="ts">
  let {name, dark, custom, reference, inactive = false, title = ''}: {
    name:string; dark:boolean; custom:boolean; reference:string; inactive?:boolean; title?:string;
  } = $props();
  // Owned masters are also the input to the engine's embedded raster package.
  const vectors = import.meta.glob<string>('../../../assets/toolbar-icons/{light,dark}/{new,open,save,copy,print,undo,redo,global-search,meta-configuration,meta-common,meta-constant,meta-catalog,meta-document,meta-enumeration,meta-report,meta-processor}.svg', {eager:true,query:'?url',import:'default'});
  // Stock examples are viewports into existing, sanitized documentation captures.
  // No extracted platform PNGs or resource containers are bundled by the UI.
  const positions:Record<string,[number,number]> = {
    new:[12,59],open:[36,59],save:[60,59],copy:[106,59],print:[156,59],
    undo:[206,59],redo:[228,59],'global-search':[260,59],
    'meta-configuration':[6,171],'meta-common':[24,190],
    'meta-constant':[24,209],'meta-catalog':[24,228],'meta-document':[24,247],
    'meta-enumeration':[24,285],'meta-report':[24,304],'meta-processor':[24,323],
  };
  const vector = $derived(vectors[`../../../assets/toolbar-icons/${dark?'dark':'light'}/${name}.svg`]);
  const position = $derived(positions[name]);
</script>

<span class="preview-icon" {title} aria-hidden="true">
  {#if custom}
    <img src={vector} alt="" width="16" height="16" style:opacity={inactive ? (dark ? 0.42 : 0.62) : 1}/>
  {:else if position}
    <span class="stock-icon" style:background-image={`url("${reference}")`} style:background-position={`-${position[0]}px -${position[1]}px`}></span>
  {/if}
</span>

<style>
  .preview-icon,.stock-icon {display:inline-block;width:16px;height:16px;flex:none;vertical-align:middle;}
  .preview-icon img {display:block;width:16px;height:16px;}
  .stock-icon {display:block;background-repeat:no-repeat;background-size:1103px 623px;}
</style>
