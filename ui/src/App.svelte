<script lang="ts">
  import { onMount } from 'svelte';
  import ConfiguratorPreview from './lib/ConfiguratorPreview.svelte';
  import brandMark from './assets/brand-mark.svg';
  import { invoke } from '@tauri-apps/api/core';
  import { Palette, Monitor, Settings2, Check, Search, Sun, Moon, Power, FolderOpen, RefreshCw, TriangleAlert, CheckCircle2, ChevronRight, ChevronDown, Minus, Plus } from '@lucide/svelte';
  type Theme = {id:string;name:string;dark:boolean;colors:Record<string,string>;editor:Record<string,string>};
  type FontSettings = {enabled:boolean;family:string;points10:number};
  type UiFontSettings = {enabled:boolean;family:string};
  type Preferences = {editor_font:FontSettings;ui_font:UiFontSettings;selected_theme:string;auto_apply:boolean;custom_icons:boolean;window:{width:number;height:number;maximized:boolean}};
  type ProcessRow = {pid:number;created_time:string|null;title:string;exe:string|null;command_line:string|null;eligible:boolean;status:string;theme:string|null;detail:string|null;icons:string|null;icon_warning:string|null;font:string|null;font_warning:string|null;ui_font:string|null;ui_font_warning:string|null};
  type AppState = {settings:{path:string;settings:Preferences;warning:string|null}|null;processes:ProcessRow[];error:string|null;busy:boolean;stopping:boolean;version:string;autostart_enabled:boolean|null;autostart_error:string|null;custom_profile:boolean};
  let appState = $state<AppState|null>(null);
  let themes = $state<Theme[]>([]);
  let fonts = $state<string[]>([]);
  let page = $state<'themes'|'processes'|'settings'>('themes');
  let filter = $state<'all'|'dark'|'light'>('all');
  let search = $state('');
  let saving = $state(false);
  let error = $state('');
  let notice = $state('');
  let serial = 0;
  const standard:Theme={id:'standard',name:'Стандартная',dark:false,colors:{'editor.background':'#fff','text':'#242424','panel':'#f5f2dd','border':'#d0ccba','selection.background':'#536ac2','selection.text':'#fff','comment':'#008000','keyword':'#ff0000','string':'#000','directive':'#963200','accent':'#0000ff','hover':'#fffad9'},editor:{keyword:'#FF0000',punctuation:'#FF0000',identifier:'#0000FF',builtin:'#0000FF',string:'#000000',number:'#000000',comment:'#008000',directive:'#963200'}};
  const catalog = $derived([standard, ...themes]);
  const selectedId = $derived(appState?.settings?.settings.selected_theme ?? 'dracula');
  const selected = $derived(catalog.find(t=>t.id===selectedId) ?? themes[2] ?? standard);
  const editor = $derived(selected.editor);
  const customIcons = $derived(appState?.settings?.settings.custom_icons ?? false);
  const font = $derived(appState?.settings?.settings.editor_font ?? {enabled:false,family:'JetBrains Mono Medium',points10:130});
  const fontFamily = $derived(`${CSS.escape(font.family)}, monospace`);
  const uiFont = $derived(appState?.settings?.settings.ui_font ?? {enabled:true,family:'Segoe UI Variable Small'});
  const uiFontFamily = $derived(uiFont.enabled?`${CSS.escape(uiFont.family)}, "Segoe UI", sans-serif`:'"Microsoft Sans Serif", Tahoma, sans-serif');
  const recommendedUiFonts = $derived(['Segoe UI Variable Small','Segoe UI','Roboto'].filter(family=>fonts.includes(family)));
  const otherUiFonts = $derived(fonts.filter(family=>!recommendedUiFonts.includes(family)).sort((a,b)=>a.localeCompare(b)));
  const auto = $derived(appState?.settings?.settings.auto_apply ?? false);
  const eligible = $derived(appState?.processes.filter(p=>p.eligible).length ?? 0);
  const disabled = $derived(saving || !!appState?.busy || !!appState?.stopping || !appState?.settings);
  const filtered = $derived(catalog.filter(t=>(filter==='all'||t.dark===(filter==='dark'))&&t.name.toLocaleLowerCase().includes(search.toLocaleLowerCase())));
  function themeName(id:string|null){return catalog.find(t=>t.id===id)?.name ?? id ?? 'Не подключён';}
  async function refresh(){const id=++serial;const result=await invoke<AppState>('get_state');if(id===serial)appState=result;}
  async function perform(action:()=>Promise<unknown>,message=''){
    saving=true;error='';notice='';
    try {await action();await refresh();notice=message;}catch(e){error=String(e);}finally{saving=false;}
  }
  async function select(id:string){
    await perform(()=>invoke('set_appearance',{patch:{selected_theme:id}}));
    if(!error)window.scrollTo({top:0,behavior:window.matchMedia('(prefers-reduced-motion: reduce)').matches?'auto':'smooth'});
  }
  function toggleIcons(){return perform(()=>invoke('set_appearance',{patch:{custom_icons:!customIcons}}));}
  function saveFont(patch:Partial<FontSettings>){return perform(()=>invoke('set_appearance',{patch:{editor_font:{...font,...patch}}}));}
  function saveUiFont(family:string){return perform(()=>invoke('set_appearance',{patch:{ui_font:family?{enabled:true,family}:{...uiFont,enabled:false}}}));}
  function toggleAuto(){return perform(()=>invoke('save_settings',{patch:{auto_apply:!auto}}),auto?'Авто-применение выключено':'Авто-применение включено');}
  function apply(row?:ProcessRow){return perform(()=>invoke('apply_theme',{pid:row?.pid??null,createdTime:row?.created_time??null}));}
  onMount(()=>{
    let disposed=false;
    const reportError=(e:unknown)=>{if(!disposed)error=String(e);};
    invoke<Theme[]>('get_themes').then(catalog=>{if(!disposed)themes=catalog;}).catch(reportError);
    invoke<string[]>('get_fonts').then(installed=>{if(!disposed)fonts=installed;}).catch(reportError);
    refresh().catch(reportError);
    const timer=setInterval(()=>{if(!disposed)refresh().catch(reportError);},1000);
    return()=>{disposed=true;clearInterval(timer);};
  });
</script>

<div class="app-shell">
  <aside class="sidebar">
    <div class="brand"><img class="brand-icon" src={brandMark} alt=""/><div>v8-chroma<span>Темы 1С</span></div></div>
    <nav aria-label="Разделы приложения">
      <button class:active={page==='themes'} onclick={()=>page='themes'} title="Темы"><Palette size={18}/><span>Темы</span></button>
      <button class:active={page==='processes'} onclick={()=>page='processes'} title="Конфигураторы"><Monitor size={18}/><span>Конфигураторы</span><b>{eligible}</b></button>
      <button class:active={page==='settings'} onclick={()=>page='settings'} title="Настройки"><Settings2 size={18}/><span>Настройки</span></button>
    </nav>
    <div class="sidebar-bottom">
      <div class="live-status"><i class:on={auto && !appState?.error}></i><span>{appState?.stopping?'Завершаем работу':appState?.error?'Наблюдение приостановлено':auto?'Наблюдение включено':'Авто-применение выключено'}</span></div>
      <div class="version">v{appState?.version??'0.3.1'}</div>
    </div>
  </aside>
  <main>
    {#if error || appState?.error || appState?.settings?.warning}
      <div class="alert" role="alert"><TriangleAlert size={18}/><div>{error || appState?.error || appState?.settings?.warning}</div></div>
    {/if}
    {#if page==='themes'}
      <header class="page-header"><h1>Темы</h1></header>
      <section class="hero appearance-hero" aria-label="Предпросмотр выбранной темы">
        <div class="appearance-header">
          <div class="current-theme"><h2>{selected.name}</h2><span class="kind-tag">{#if selected.dark}<Moon size={12}/>{:else}<Sun size={12}/>{/if}{selected.dark?'Тёмная':'Светлая'}</span></div>
          <div class="swatches" aria-hidden="true">{#each ['keyword','identifier','comment','string'] as role}<i style:background={editor[role]}></i>{/each}</div>
        </div>
        <div class="appearance-controls">
          <div class="appearance-field font-family-control ui-font-control"><label for="ui-font-family">Шрифт интерфейса</label><div class="font-select"><select id="ui-font-family" aria-label="Шрифт интерфейса" value={uiFont.enabled?uiFont.family:''} disabled={disabled} onchange={event=>saveUiFont(event.currentTarget.value)}><option value="">Как в 1С</option>{#if uiFont.enabled && !fonts.includes(uiFont.family)}<option value={uiFont.family} disabled>{uiFont.family} — не установлен</option>{/if}<optgroup label="Рекомендуемые">{#each recommendedUiFonts as family}<option value={family}>{family}</option>{/each}</optgroup><optgroup label="Другие установленные шрифты">{#each otherUiFonts as family}<option value={family}>{family}</option>{/each}</optgroup></select><ChevronDown size={15} aria-hidden="true"/></div></div>
          <div class="appearance-field font-family-control"><label for="font-family">Шрифт редактора</label><div class="font-select"><select id="font-family" aria-label="Шрифт редактора" value={font.enabled?font.family:''} disabled={disabled || fonts.length===0} onchange={event=>saveFont(event.currentTarget.value?{enabled:true,family:event.currentTarget.value}:{enabled:false})}><option value="">Как в 1С</option>{#if font.enabled && !fonts.includes(font.family)}<option value={font.family} disabled>{font.family} — не установлен</option>{/if}{#each fonts as family}<option value={family}>{family}</option>{/each}</select><ChevronDown size={15} aria-hidden="true"/></div></div>
          <div class="appearance-field"><label for="font-size">Размер, pt</label><div class="font-stepper" class:inactive={!font.enabled}><button aria-label="Уменьшить размер шрифта" disabled={disabled || !font.enabled || font.points10<=80} onclick={()=>saveFont({points10:font.points10-5})}><Minus size={15}/></button><input id="font-size" aria-label="Размер шрифта" type="number" min="8" max="32" step="0.5" placeholder="1С" value={font.enabled?font.points10/10:undefined} disabled={disabled || !font.enabled} onchange={event=>saveFont({points10:Math.round(Number(event.currentTarget.value)*10)})}/><button aria-label="Увеличить размер шрифта" disabled={disabled || !font.enabled || font.points10>=320} onclick={()=>saveFont({points10:font.points10+5})}><Plus size={15}/></button></div></div>
          <div class="appearance-field"><label for="custom-icons">Значки</label><button id="custom-icons" class="appearance-toggle" role="switch" aria-checked={customIcons} aria-label="Новые значки конфигуратора" disabled={disabled} onclick={toggleIcons}><span>{customIcons?'Новые':'Штатные'}</span><span class="switch" class:checked={customIcons} aria-hidden="true"><i></i></span></button></div>
        </div>
        <ConfiguratorPreview theme={selected} {customIcons} {uiFontFamily} fontFamily={font.enabled?fontFamily:undefined} fontSize={font.enabled?font.points10/10:undefined}/>
        {#if !auto}<div class="appearance-apply"><button class="primary" disabled={disabled || eligible===0} onclick={()=>apply()}><Check size={15}/>Применить оформление</button></div>{/if}
      </section>
      <section class="collection" aria-label="Коллекция тем">
        <div class="collection-header"><div class="filters" aria-label="Фильтр тем"><button class:chosen={filter==='all'} onclick={()=>filter='all'}>Все<span>{catalog.length}</span></button><button class:chosen={filter==='dark'} onclick={()=>filter='dark'}><Moon size={13}/>Тёмные</button><button class:chosen={filter==='light'} onclick={()=>filter='light'}><Sun size={13}/>Светлые</button></div><label class="search"><Search size={15}/><input aria-label="Поиск темы" placeholder="Найти тему…" bind:value={search}/></label></div>
        <div class="theme-grid">
          {#each filtered as t}
            <button class="theme-card" class:selected={selectedId===t.id} aria-pressed={selectedId===t.id} disabled={disabled} onclick={()=>select(t.id)} title={t.name}>
              <div class="mini-editor" style:background={t.colors['editor.background']}><i style:background={t.colors.keyword}></i><i style:background={t.colors.text}></i><i style:background={t.colors.string}></i><i style:background={t.colors.comment}></i><i style:background={t.colors.accent}></i></div>
              <div class="theme-card-label"><strong>{t.name}</strong><small>{t.dark?'Тёмная':'Светлая'}</small></div><span class="selection-check">{#if selectedId===t.id}<Check size={13}/>{/if}</span>
            </button>
          {/each}
        </div>
        {#if filtered.length===0}<div class="no-results">Такая тема не найдена. Попробуйте другое название.</div>{/if}
      </section>
    {:else if page==='processes'}
      <header class="page-header"><h1>Конфигураторы</h1><button class="icon-button" title="Обновить список" aria-label="Обновить список" onclick={()=>perform(()=>invoke('refresh_processes'))}><RefreshCw size={17}/></button></header>
      <div class="process-summary"><div><span class="big-number">{eligible}</span><div><strong>Доступно для подключения</strong></div></div><button class="primary" disabled={disabled || eligible===0} onclick={()=>apply()}><Check size={15}/>Применить ко всем</button></div>
      {#if !appState?.processes.length}
        <div class="empty-state"><div class="empty-icon"><Monitor size={30}/></div><h2>Конфигураторы пока не открыты</h2><p>Запустите конфигуратор 1С.<br/>Он появится здесь автоматически.</p></div>
      {:else}
        <div class="process-list">{#each appState.processes as row}<article class="process-card"><div class="process-top"><div class="process-symbol"><Monitor size={20}/></div><div class="process-name"><strong>{row.title}</strong><span>PID {row.pid}</span></div><span class="process-status" class:good={row.status==='applied'} class:bad={row.status==='error'}>{#if row.status==='applied'}<CheckCircle2 size={13}/>Применена{:else if row.status==='applying'}<RefreshCw size={13} class="spin"/>Применяем{:else if row.status==='error'}Ошибка{:else if !row.eligible}Пропущен{:else}Ожидает подключения{/if}</span></div><div class="process-command"><span>Команда запуска</span><code>{row.command_line??'Командная строка недоступна'}</code></div><div class="process-bottom"><div><small>Последнее применение</small><strong>{themeName(row.theme)}</strong>{#if row.icons}<small>{row.icons==='light'||row.icons==='dark'?'Новые значки':'Штатные значки'}</small>{/if}</div><button class="secondary" disabled={disabled || !row.eligible} onclick={()=>apply(row)}>Применить<ChevronRight size={14}/></button></div>{#if row.font}<small class="process-font">{row.font}</small>{/if}{#if row.ui_font}<small class="process-font">Интерфейс: {row.ui_font}</small>{/if}{#if row.ui_font_warning}<p class="setting-warning">{row.ui_font_warning}</p>{/if}{#if row.font_warning}<p class="setting-warning">{row.font_warning}</p>{/if}{#if row.icon_warning}<p class="setting-warning">{row.icon_warning}</p>{/if}{#if row.detail}<details><summary>{row.eligible?'Подробнее об ошибке':'Почему пропущен'}</summary><p>{row.detail}</p></details>{/if}</article>{/each}</div>
      {/if}
    {:else}
      <header class="page-header"><h1>Настройки</h1></header>
      <section class="settings-group"><h2>Работа приложения</h2><div class="setting-row"><div><strong>Автоматическое применение</strong><p>Применять выбранную тему, значки и шрифты к открытым и новым конфигураторам. Если выключено, используйте «Применить оформление» на вкладке «Темы».</p></div><button class="switch" class:checked={auto} role="switch" aria-checked={auto} aria-label="Автоматическое применение оформления" disabled={disabled} onclick={toggleAuto}><i></i></button></div><div class="setting-row"><div><strong>Запускать при входе в Windows</strong><p>{appState?.custom_profile?'Для отдельного профиля автозапуск недоступен.':'В фоне, со значком в системном трее.'}</p></div><button class="switch" class:checked={appState?.autostart_enabled===true} role="switch" aria-checked={appState?.autostart_enabled===true} aria-label="Запускать при входе в Windows" disabled={saving || appState?.custom_profile || appState?.autostart_enabled==null} onclick={()=>perform(()=>invoke('set_autostart',{enabled:!appState?.autostart_enabled}),'Настройка автозапуска сохранена')}><i></i></button></div>{#if appState?.autostart_error}<p class="setting-warning">{appState.autostart_error}</p>{/if}</section>
      <section class="settings-group"><h2>Ваши настройки</h2><div class="settings-path"><span>Расположение файла</span><code>{appState?.settings?.path??'Файл настроек недоступен'}</code><button class="secondary" onclick={()=>perform(()=>invoke('open_settings_folder'))}><FolderOpen size={15}/>Открыть папку</button></div><div class="setting-row"><div><strong>Вернуть настройки по умолчанию</strong><p>Dracula, Segoe UI Variable Small и включённое авто-применение. Предыдущая копия сохраняется.</p></div><button class="secondary" disabled={saving} onclick={()=>perform(()=>invoke('reset_settings'),'Настройки восстановлены')}>Сбросить</button></div></section>
      <div class="quit-row"><p>Крестик сворачивает приложение в трей. Для полного выхода используйте кнопку; конфигураторы останутся открытыми.</p><button class="quit-button" disabled={appState?.stopping} onclick={()=>perform(()=>invoke('quit_app'))}><Power size={15}/>{appState?.stopping?'Завершаем…':'Завершить приложение'}</button></div>
    {/if}
    {#if page==='settings' && (saving || appState?.stopping || notice)}
      <footer role="status">{saving?'Сохраняем…':appState?.stopping?'Завершаем текущую операцию…':notice}</footer>
    {/if}
  </main>
</div>
