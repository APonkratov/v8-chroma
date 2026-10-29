<script lang="ts">
  import {ChevronDown, ChevronRight, FileCode2} from '@lucide/svelte';
  import PreviewIcon from './PreviewIcon.svelte';
  import type {ThemePreview} from './preview';
  import {stockReference} from './preview';
  let {theme, customIcons}:{theme:ThemePreview;customIcons:boolean} = $props();
  const colors = $derived(theme.colors);
  const editor = $derived(theme.editor);
  const reference = $derived(stockReference(theme.id));
  const commands = [
    {icon:'new',label:'Создать'},{icon:'open',label:'Открыть'},{icon:'save',label:'Сохранить'},
    {icon:'copy',label:'Копировать'},{icon:'print',label:'Печать'},
    {icon:'undo',label:'Отменить'},{icon:'redo',label:'Повторить'},{icon:'global-search',label:'Глобальный поиск'},
  ];
  const nodes = [
    {icon:'meta-common',label:'Общие'},{icon:'meta-constant',label:'Константы'},
    {icon:'meta-catalog',label:'Справочники'},{icon:'meta-document',label:'Документы'},
    {icon:'meta-enumeration',label:'Перечисления'},{icon:'meta-report',label:'Отчёты'},
    {icon:'meta-processor',label:'Обработки'},
  ];
</script>

<div class="preview-heading">
  <span class="preview-label">Пример конфигуратора</span>
</div>
<div class="configurator-preview" style:background={colors['editor.background']} style:color={colors.text} style:--preview-panel={colors.panel} style:--preview-border={colors.border} aria-label={`Пример конфигуратора: ${theme.name}, ${customIcons?'новые':'штатные'} значки`}>
  <div class="preview-menubar" aria-hidden="true"><span>Файл</span><span>Правка</span><span>Конфигурация</span><span>Отладка</span><span>Сервис</span></div>
  <div class="preview-toolbar" aria-label="Пример панели команд">
    {#each commands as command,i}
      {#if i===3 || i===5}<span class="preview-separator"></span>{/if}
      <span class="preview-tool"><PreviewIcon name={command.icon} dark={theme.dark} custom={customIcons} {reference} inactive={['save','copy','undo','redo'].includes(command.icon)} title={command.label}/></span>
    {/each}
    <span class="preview-icon-kind">{customIcons?'Новые значки':'Штатные значки'}</span>
  </div>
  <div class="preview-workspace">
    <div class="preview-tree" aria-label="Пример дерева конфигурации">
      <div class="preview-tree-title">Конфигурация</div>
      <ul>
        <li class="preview-root" style:background={colors['selection.background']} style:color={colors['selection.text']}><ChevronDown size={10}/><PreviewIcon name="meta-configuration" dark={theme.dark} custom={customIcons} {reference}/><span>Конфигурация</span></li>
        {#each nodes as node}
          <li><ChevronRight size={9}/><PreviewIcon name={node.icon} dark={theme.dark} custom={customIcons} {reference}/><span>{node.label}</span></li>
        {/each}
      </ul>
    </div>
    <div class="preview-module">
      <div class="preview-module-tab"><FileCode2 size={13}/><span>Модуль формы</span><span class="preview-tab-dot"></span></div>
      <div class="preview-code">
        <div class="preview-numbers" aria-hidden="true">1<br/>2<br/>3<br/>4<br/>5<br/>6<br/>7<br/>8</div>
        <pre style:color={editor.identifier}><span style:color={editor.comment}>// Проверка оформления</span>
<span style:color={editor.keyword}>Процедура</span> ПроверитьТему<span style:color={editor.punctuation}>()</span>
    Тема <span style:color={editor.punctuation}>=</span> <span style:color={editor.string}>"{theme.name}"</span><span style:color={editor.punctuation}>;</span>
    <span style:color={editor.keyword}>Если</span> <span style:color={editor.number}>42</span> <span style:color={editor.punctuation}>&gt;</span> <span style:color={editor.number}>0</span> <span style:color={editor.keyword}>Тогда</span>
        <span style:color={editor.builtin}>Сообщить</span><span style:color={editor.punctuation}>(</span>Тема<span style:color={editor.punctuation}>);</span>
    <span style:color={editor.keyword}>КонецЕсли</span><span style:color={editor.punctuation}>;</span>
<span style:color={editor.keyword}>КонецПроцедуры</span>
<span style:color={editor.comment}>// Готово</span></pre>
      </div>
      <div class="preview-samples"><span style:background={colors['selection.background']} style:color={colors['selection.text']}>Выделение</span><span style:background={colors.hover}>Наведение</span></div>
    </div>
  </div>
</div>
