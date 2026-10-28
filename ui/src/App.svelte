<script lang="ts">
  import { onMount } from 'svelte';
  import brandMark from './assets/brand-mark.svg';
  import { invoke } from '@tauri-apps/api/core';
  import { Palette, Monitor, Settings2, Check, Search, Sun, Moon, Power, FolderOpen, RefreshCw, TriangleAlert, CheckCircle2, ChevronRight, FileCode2 } from '@lucide/svelte';
  type Theme = {id:string;name:string;dark:boolean;colors:Record<string,string>;editor:Record<string,string>};
  type Preferences = {selected_theme:string;auto_apply:boolean;window:{width:number;height:number;maximized:boolean}};
  type ProcessRow = {pid:number;created_time:string|null;title:string;exe:string|null;command_line:string|null;eligible:boolean;status:string;theme:string|null;detail:string|null};
  type AppState = {settings:{path:string;settings:Preferences;warning:string|null}|null;processes:ProcessRow[];error:string|null;busy:boolean;stopping:boolean;version:string;autostart_enabled:boolean|null;autostart_error:string|null;custom_profile:boolean};
  let appState = $state<AppState|null>(null);
  let themes = $state<Theme[]>([]);
  let page = $state<'themes'|'processes'|'settings'>('themes');
  let filter = $state<'all'|'dark'|'light'>('all');
  let search = $state('');
  let saving = $state(false);
  let error = $state('');
  let notice = $state('');
  let serial = 0;
  const standard:Theme={id:'diagnostic',name:'Стандартная',dark:false,colors:{'editor.background':'#fff','text':'#242424','panel':'#f5f2dd','border':'#d0ccba','selection.background':'#536ac2','selection.text':'#fff','comment':'#008000','keyword':'#ff0000','string':'#000','directive':'#963200','accent':'#0000ff','hover':'#fffad9'},editor:{keyword:'#FF0000',punctuation:'#FF0000',identifier:'#0000FF',builtin:'#0000FF',string:'#000000',number:'#000000',comment:'#008000',directive:'#963200'}};
  const catalog = $derived([standard, ...themes]);
  const selectedId = $derived(appState?.settings?.settings.selected_theme ?? 'dracula');
  const selected = $derived(catalog.find(t=>t.id===selectedId) ?? themes[2] ?? standard);
  const colors = $derived(selected.colors);
  const editor = $derived(selected.editor);
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
  function select(id:string){return perform(()=>invoke('save_settings',{patch:{selected_theme:id}}));}
  function toggleAuto(){return perform(()=>invoke('save_settings',{patch:{auto_apply:!auto}}),auto?'Авто-применение выключено':'Авто-применение включено');}
  function apply(row?:ProcessRow){return perform(()=>invoke('apply_theme',{pid:row?.pid??null,createdTime:row?.created_time??null}));}
  onMount(()=>{
    let disposed=false;
    Promise.all([invoke<Theme[]>('get_themes'),refresh()]).then(([catalog])=>{if(!disposed)themes=catalog;}).catch(e=>error=String(e));
    const timer=setInterval(()=>{if(!disposed)refresh().catch(e=>error=String(e));},1000);
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
      <div class="version">v{appState?.version??'0.1.0'}</div>
    </div>
  </aside>
  <main>
    {#if error || appState?.error || appState?.settings?.warning}
      <div class="alert" role="alert"><TriangleAlert size={18}/><div>{error || appState?.error || appState?.settings?.warning}</div></div>
    {/if}
    {#if page==='themes'}
      <header class="page-header"><h1>Темы</h1></header>
      <section class="hero" aria-label="Предпросмотр выбранной темы">
        <div class="editor" style:background={colors['editor.background']} style:color={colors.text}>
          <div class="editor-tab" style:background={colors.panel}><FileCode2 size={13}/><span>Модуль формы</span><span class="editor-dot"></span></div>
          <div class="code-content">
            <div class="numbers">1<br/>2<br/>3<br/>4<br/>5<br/>6<br/>7<br/>8</div>
            <pre style:color={editor.identifier}><span style:color={editor.comment}>// Проверка палитры</span>
<span style:color={editor.keyword}>Процедура</span> ПроверитьПалитру<span style:color={editor.punctuation}>()</span>
    НазваниеТемы <span style:color={editor.punctuation}>=</span> <span style:color={editor.string}>"Новая палитра"</span><span style:color={editor.punctuation}>;</span>
    Количество <span style:color={editor.punctuation}>=</span> <span style:color={editor.number}>42</span><span style:color={editor.punctuation}>;</span>
    <span style:color={editor.keyword}>Если</span> Количество <span style:color={editor.punctuation}>&gt;</span> <span style:color={editor.number}>0</span> <span style:color={editor.keyword}>Тогда</span>
        <span style:color={editor.builtin}>Сообщить</span><span style:color={editor.punctuation}>(</span>НазваниеТемы<span style:color={editor.punctuation}>);</span>
    <span style:color={editor.keyword}>КонецЕсли</span><span style:color={editor.punctuation}>;</span>
<span style:color={editor.keyword}>КонецПроцедуры</span></pre>
          </div>
          <div class="editor-samples"><span style:background={colors['selection.background']} style:color={colors['selection.text']}>Выделение</span><span style:background={colors.hover} style:color={colors.text}>Наведение</span><small>Предпросмотр палитры</small></div>
        </div>
        <div class="current-theme"><h2>{selected.name}</h2><span class="kind-tag">{#if selected.dark}<Moon size={12}/>{:else}<Sun size={12}/>{/if}{selected.dark?'Тёмная':'Светлая'}</span><div class="swatches">{#each ['keyword','identifier','comment','string'] as role}<i style:background={editor[role]}></i>{/each}</div>
          <button class="primary" disabled={disabled || eligible===0} onclick={()=>apply()}>{#if appState?.busy}<RefreshCw size={14} class="spin"/>{:else}<Check size={15}/>{/if}Применить сейчас</button>
        </div>
      </section>
      <p class="preview-note">Для стандартных цветов редактора 1С. Строки и числа пока используют общий цвет.</p>
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
        <div class="process-list">{#each appState.processes as row}<article class="process-card"><div class="process-top"><div class="process-symbol"><Monitor size={20}/></div><div class="process-name"><strong>{row.title}</strong><span>PID {row.pid}</span></div><span class="process-status" class:good={row.status==='applied'} class:bad={row.status==='error'}>{#if row.status==='applied'}<CheckCircle2 size={13}/>Применена{:else if row.status==='applying'}<RefreshCw size={13} class="spin"/>Применяем{:else if row.status==='error'}Ошибка{:else if !row.eligible}Пропущен{:else}Ожидает подключения{/if}</span></div><div class="process-command"><span>Команда запуска</span><code>{row.command_line??'Командная строка недоступна'}</code></div><div class="process-bottom"><div><small>Последнее применение</small><strong>{themeName(row.theme)}</strong></div><button class="secondary" disabled={disabled || !row.eligible} onclick={()=>apply(row)}>Применить<ChevronRight size={14}/></button></div>{#if row.detail}<details><summary>{row.eligible?'Подробнее об ошибке':'Почему пропущен'}</summary><p>{row.detail}</p></details>{/if}</article>{/each}</div>
      {/if}
    {:else}
      <header class="page-header"><h1>Настройки</h1></header>
      <section class="settings-group"><h2>Работа приложения</h2><div class="setting-row"><div><strong>Автоматическое применение</strong><p>Оформлять открытые и новые конфигураторы.</p></div><button class="switch" class:checked={auto} role="switch" aria-checked={auto} aria-label="Автоматическое применение" disabled={saving || !appState?.settings} onclick={toggleAuto}><i></i></button></div><div class="setting-row"><div><strong>Запускать при входе в Windows</strong><p>{appState?.custom_profile?'Для отдельного профиля автозапуск недоступен.':'В фоне, со значком в системном трее.'}</p></div><button class="switch" class:checked={appState?.autostart_enabled===true} role="switch" aria-checked={appState?.autostart_enabled===true} aria-label="Запускать при входе в Windows" disabled={saving || appState?.custom_profile || appState?.autostart_enabled==null} onclick={()=>perform(()=>invoke('set_autostart',{enabled:!appState?.autostart_enabled}),'Настройка автозапуска сохранена')}><i></i></button></div>{#if appState?.autostart_error}<p class="setting-warning">{appState.autostart_error}</p>{/if}</section>
      <section class="settings-group"><h2>Ваши настройки</h2><div class="settings-path"><span>Расположение файла</span><code>{appState?.settings?.path??'Файл настроек недоступен'}</code><button class="secondary" onclick={()=>perform(()=>invoke('open_settings_folder'))}><FolderOpen size={15}/>Открыть папку</button></div><div class="setting-row"><div><strong>Вернуть настройки по умолчанию</strong><p>Dracula и включённое авто-применение. Предыдущая копия сохраняется.</p></div><button class="secondary" disabled={saving} onclick={()=>perform(()=>invoke('reset_settings'),'Настройки восстановлены')}>Сбросить</button></div></section>
      <div class="quit-row"><p>Крестик сворачивает приложение в трей. Для полного выхода используйте кнопку; конфигураторы останутся открытыми.</p><button class="quit-button" disabled={appState?.stopping} onclick={()=>perform(()=>invoke('quit_app'))}><Power size={15}/>{appState?.stopping?'Завершаем…':'Завершить приложение'}</button></div>
    {/if}
    {#if page==='settings' && (saving || appState?.stopping || notice)}
      <footer role="status">{saving?'Сохраняем…':appState?.stopping?'Завершаем текущую операцию…':notice}</footer>
    {/if}
  </main>
</div>
