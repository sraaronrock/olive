/***

    Olive - Non-Linear Video Editor
    Copyright (C) 2019  Olive Team

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "effectcontrols.h"

#include <QMenu>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScrollArea>
#include <QPushButton>
#include <QSpacerItem>
#include <QLabel>
#include <QVBoxLayout>
#include <QIcon>
#include <QApplication>

#include "panels/panels.h"
#include "project/effect.h"
#include "project/clip.h"
#include "project/transition.h"
#include "ui/collapsiblewidget.h"
#include "project/sequence.h"
#include "project/undo.h"
#include "panels/project.h"
#include "panels/timeline.h"
#include "panels/viewer.h"
#include "panels/grapheditor.h"
#include "ui/viewerwidget.h"
#include "ui/menuhelper.h"
#include "io/clipboard.h"
#include "io/config.h"
#include "ui/timelineheader.h"
#include "ui/keyframeview.h"
#include "ui/resizablescrollbar.h"
#include "debug.h"

EffectControls::EffectControls(QWidget *parent) :
  Panel(parent),
  zoom(1),
  mode_(kTransitionNone)
{
  setup_ui();
  Retranslate();

  Clear(false);

  headers->viewer = panel_sequence_viewer;
  headers->snapping = false;

  effects_area->parent_widget = scrollArea;
  effects_area->keyframe_area = keyframeView;
  effects_area->header = headers;
  keyframeView->header = headers;

  connect(keyframeView, SIGNAL(wheel_event_signal(QWheelEvent*)), effects_area, SLOT(receive_wheel_event(QWheelEvent*)));
  connect(horizontalScrollBar, SIGNAL(valueChanged(int)), headers, SLOT(set_scroll(int)));
  connect(horizontalScrollBar, SIGNAL(resize_move(double)), keyframeView, SLOT(resize_move(double)));
  connect(horizontalScrollBar, SIGNAL(valueChanged(int)), keyframeView, SLOT(set_x_scroll(int)));
  connect(verticalScrollBar, SIGNAL(valueChanged(int)), keyframeView, SLOT(set_y_scroll(int)));
  connect(verticalScrollBar, SIGNAL(valueChanged(int)), scrollArea->verticalScrollBar(), SLOT(setValue(int)));
  connect(scrollArea->verticalScrollBar(), SIGNAL(valueChanged(int)), verticalScrollBar, SLOT(setValue(int)));
}

EffectControls::~EffectControls()
{
  Clear(true);
}

bool EffectControls::keyframe_focus() {
  return headers->hasFocus() || keyframeView->hasFocus();
}

void EffectControls::set_zoom(bool in) {
  zoom *= (in) ? 2 : 0.5;
  update_keyframes();
  scroll_to_frame(olive::ActiveSequence->playhead);
}

void EffectControls::menu_select(QAction* q) {
  ComboAction* ca = new ComboAction();
  for (int i=0;i<selected_clips_.size();i++) {
    Clip* c = selected_clips_.at(i);
    if ((c->track() < 0) == (effect_menu_subtype == EFFECT_TYPE_VIDEO)) {
      const EffectMeta* meta = reinterpret_cast<const EffectMeta*>(q->data().value<quintptr>());
      if (effect_menu_type == EFFECT_TYPE_TRANSITION) {
        if (c->opening_transition == nullptr) {
          ca->append(new AddTransitionCommand(c,
                                              nullptr,
                                              nullptr,
                                              meta,
                                              olive::CurrentConfig.default_transition_length));
        }
        if (c->closing_transition == nullptr) {
          ca->append(new AddTransitionCommand(nullptr,
                                              c,
                                              nullptr,
                                              meta,
                                              olive::CurrentConfig.default_transition_length));
        }
      } else {
        ca->append(new AddEffectCommand(c, nullptr, meta));
      }
    }
  }
  olive::UndoStack.push(ca);
  if (effect_menu_type == EFFECT_TYPE_TRANSITION) {
    update_ui(true);
  } else {
    Reload();
    panel_sequence_viewer->viewer_widget->frame_update();
  }
}

void EffectControls::update_keyframes() {
  headers->update_zoom(zoom);
  keyframeView->update();
}

void EffectControls::delete_selected_keyframes() {
  keyframeView->delete_selected_keyframes();
}

void EffectControls::copy(bool del) {
  if (mode_ == kTransitionNone) {
    bool cleared = false;

    ComboAction* ca = nullptr;
    if (del) {
      ca = new ComboAction();
    }

    for (int i=0;i<open_effects_.size();i++) {
      if (open_effects_.at(i)->IsSelected()) {
        Effect* e = open_effects_.at(i)->GetEffect();

        if (!cleared) {
          clear_clipboard();
          cleared = true;
          clipboard_type = CLIPBOARD_TYPE_EFFECT;
        }

        clipboard.append(e->copy(nullptr));

        if (del) {
          ca->append(new EffectDeleteCommand(e));
        }
      }
    }

    if (del) {
      if (ca->hasActions()) {
        olive::UndoStack.push(ca);
      } else {
        delete ca;
      }
    }
  }
}

void EffectControls::scroll_to_frame(long frame) {
  scroll_to_frame_internal(horizontalScrollBar, frame - keyframeView->visible_in, zoom, keyframeView->width());
}

void EffectControls::cut() {
  copy(true);
}

void EffectControls::show_effect_menu(int type, int subtype) {
  effect_menu_type = type;
  effect_menu_subtype = subtype;

  effects_loaded.lock();

  QMenu effects_menu(this);
  effects_menu.setToolTipsVisible(true);

  for (int i=0;i<effects.size();i++) {
    const EffectMeta& em = effects.at(i);

    if (em.type == type && em.subtype == subtype) {
      QAction* action = new QAction(&effects_menu);
      action->setText(em.name);
      action->setData(reinterpret_cast<quintptr>(&em));
      if (!em.tooltip.isEmpty()) {
        action->setToolTip(em.tooltip);
      }

      QMenu* parent = &effects_menu;
      if (!em.category.isEmpty()) {
        bool found = false;
        for (int j=0;j<effects_menu.actions().size();j++) {
          QAction* action = effects_menu.actions().at(j);
          if (action->menu() != nullptr) {
            if (action->menu()->title() == em.category) {
              parent = action->menu();
              found = true;
              break;
            }
          }
        }
        if (!found) {
          parent = new QMenu(&effects_menu);
          parent->setToolTipsVisible(true);
          parent->setTitle(em.category);

          bool found = false;
          for (int i=0;i<effects_menu.actions().size();i++) {
            QAction* comp_action = effects_menu.actions().at(i);
            if (comp_action->text() > em.category) {
              effects_menu.insertMenu(comp_action, parent);
              found = true;
              break;
            }
          }
          if (!found) effects_menu.addMenu(parent);
        }
      }

      bool found = false;
      for (int i=0;i<parent->actions().size();i++) {
        QAction* comp_action = parent->actions().at(i);
        if (comp_action->text() > action->text()) {
          parent->insertAction(comp_action, action);
          found = true;
          break;
        }
      }
      if (!found) parent->addAction(action);
    }
  }

  effects_loaded.unlock();

  connect(&effects_menu, SIGNAL(triggered(QAction*)), this, SLOT(menu_select(QAction*)));
  effects_menu.exec(QCursor::pos());
}

void EffectControls::Clear(bool clear_cache) {
  // clear existing clips
  deselect_all_effects(nullptr);

  // clear graph editor
  if (panel_graph_editor != nullptr) panel_graph_editor->set_row(nullptr);

  for (int i=0;i<open_effects_.size();i++) {
    delete open_effects_.at(i);
  }
  open_effects_.clear();

  vcontainer->setVisible(false);
  acontainer->setVisible(false);
  headers->setVisible(false);
  keyframeView->setEnabled(false);

  if (clear_cache) {
    selected_clips_.clear();
  }

  UpdateTitle();
}

bool EffectControls::IsEffectSelected(Effect *e)
{
  for (int i=0;i<open_effects_.size();i++) {
    if (open_effects_.at(i)->GetEffect() == e && open_effects_.at(i)->IsSelected()) {
      return true;
    }
  }
  return false;
}

void EffectControls::deselect_all_effects(QWidget* sender) {

  for (int i=0;i<open_effects_.size();i++) {
    if (open_effects_.at(i) != sender) {
      open_effects_.at(i)->header_click(false, false);
    }
  }

  panel_sequence_viewer->viewer_widget->update();
}

void EffectControls::open_effect(QVBoxLayout* layout, Effect* e) {
  EffectUI* container = new EffectUI(e);

  connect(container, SIGNAL(CutRequested()), this, SLOT(cut()));
  connect(container, SIGNAL(CopyRequested()), this, SLOT(copy(bool)));
  connect(container, SIGNAL(deselect_others(QWidget*)), this, SLOT(deselect_all_effects(QWidget*)));

  open_effects_.append(container);

  layout->addWidget(container);
}

void EffectControls::UpdateTitle() {
  if (selected_clips_.isEmpty()) {
    setWindowTitle(panel_name + tr("(none)"));
  } else {
    setWindowTitle(panel_name + selected_clips_.first()->name());
  }
}

void EffectControls::setup_ui() {
  QWidget* contents = new QWidget(this);

  QHBoxLayout* hlayout = new QHBoxLayout(contents);
  hlayout->setSpacing(0);
  hlayout->setMargin(0);

  splitter = new QSplitter();
  splitter->setOrientation(Qt::Horizontal);
  splitter->setChildrenCollapsible(false);

  scrollArea = new QScrollArea();
  scrollArea->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);
  scrollArea->setFrameShape(QFrame::NoFrame);
  scrollArea->setFrameShadow(QFrame::Plain);
  scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  scrollArea->setWidgetResizable(true);

  QWidget* scrollAreaWidgetContents = new QWidget();

  QHBoxLayout* scrollAreaLayout = new QHBoxLayout(scrollAreaWidgetContents);
  scrollAreaLayout->setSpacing(0);
  scrollAreaLayout->setMargin(0);

  effects_area = new EffectsArea();
  effects_area->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(effects_area, SIGNAL(customContextMenuRequested(const QPoint &)), this, SLOT(effects_area_context_menu()));

  QVBoxLayout* effects_area_layout = new QVBoxLayout(effects_area);
  effects_area_layout->setSpacing(0);
  effects_area_layout->setMargin(0);

  vcontainer = new QWidget();
  QVBoxLayout* vcontainerLayout = new QVBoxLayout(vcontainer);
  vcontainerLayout->setSpacing(0);
  vcontainerLayout->setMargin(0);

  QWidget* veHeader = new QWidget();
  veHeader->setObjectName(QStringLiteral("veHeader"));
  veHeader->setStyleSheet(QLatin1String("#veHeader { background: rgba(0, 0, 0, 0.25); }"));

  QHBoxLayout* veHeaderLayout = new QHBoxLayout(veHeader);
  veHeaderLayout->setSpacing(0);
  veHeaderLayout->setMargin(0);

  QIcon add_effect_icon(":/icons/add-effect.svg");
  QIcon add_transition_icon(":/icons/add-transition.svg");

  btnAddVideoEffect = new QPushButton();
  btnAddVideoEffect->setIcon(add_effect_icon);
  veHeaderLayout->addWidget(btnAddVideoEffect);
  connect(btnAddVideoEffect, SIGNAL(clicked(bool)), this, SLOT(video_effect_click()));

  veHeaderLayout->addStretch();

  lblVideoEffects = new QLabel();
  QFont font;
  font.setPointSize(9);
  lblVideoEffects->setFont(font);
  lblVideoEffects->setAlignment(Qt::AlignCenter);
  veHeaderLayout->addWidget(lblVideoEffects);

  veHeaderLayout->addStretch();

  btnAddVideoTransition = new QPushButton();
  btnAddVideoTransition->setIcon(add_transition_icon);
  connect(btnAddVideoTransition, SIGNAL(clicked(bool)), this, SLOT(video_transition_click()));
  veHeaderLayout->addWidget(btnAddVideoTransition);

  vcontainerLayout->addWidget(veHeader);

  video_effect_area = new QWidget();
  video_effect_layout = new QVBoxLayout(video_effect_area);
  video_effect_layout->setSpacing(0);
  video_effect_layout->setMargin(0);

  vcontainerLayout->addWidget(video_effect_area);

  effects_area_layout->addWidget(vcontainer);

  acontainer = new QWidget();
  QVBoxLayout* acontainerLayout = new QVBoxLayout(acontainer);
  acontainerLayout->setSpacing(0);
  acontainerLayout->setMargin(0);
  QWidget* aeHeader = new QWidget();
  aeHeader->setObjectName(QStringLiteral("aeHeader"));
  aeHeader->setStyleSheet(QLatin1String("#aeHeader { background: rgba(0, 0, 0, 0.25); }"));

  QHBoxLayout* aeHeaderLayout = new QHBoxLayout(aeHeader);
  aeHeaderLayout->setSpacing(0);
  aeHeaderLayout->setMargin(0);

  btnAddAudioEffect = new QPushButton();
  btnAddAudioEffect->setIcon(add_effect_icon);
  connect(btnAddAudioEffect, SIGNAL(clicked(bool)), this, SLOT(audio_effect_click()));
  aeHeaderLayout->addWidget(btnAddAudioEffect);

  aeHeaderLayout->addStretch();

  lblAudioEffects = new QLabel();
  lblAudioEffects->setFont(font);
  lblAudioEffects->setAlignment(Qt::AlignCenter);
  aeHeaderLayout->addWidget(lblAudioEffects);

  aeHeaderLayout->addStretch();

  btnAddAudioTransition = new QPushButton();
  btnAddAudioTransition->setIcon(add_transition_icon);
  connect(btnAddAudioTransition, SIGNAL(clicked(bool)), this, SLOT(audio_transition_click()));
  aeHeaderLayout->addWidget(btnAddAudioTransition);

  acontainerLayout->addWidget(aeHeader);

  audio_effect_area = new QWidget();
  audio_effect_layout = new QVBoxLayout(audio_effect_area);
  audio_effect_layout->setSpacing(0);
  audio_effect_layout->setMargin(0);

  acontainerLayout->addWidget(audio_effect_area);

  effects_area_layout->addWidget(acontainer);

  effects_area_layout->addStretch();

  scrollAreaLayout->addWidget(effects_area);

  scrollArea->setWidget(scrollAreaWidgetContents);
  splitter->addWidget(scrollArea);

  QWidget* keyframeArea = new QWidget();

  QSizePolicy keyframe_sp;
  keyframe_sp.setHorizontalPolicy(QSizePolicy::Minimum);
  keyframe_sp.setVerticalPolicy(QSizePolicy::Preferred);
  keyframe_sp.setHorizontalStretch(1);
  keyframeArea->setSizePolicy(keyframe_sp);

  QVBoxLayout* keyframeAreaLayout = new QVBoxLayout(keyframeArea);
  keyframeAreaLayout->setSpacing(0);
  keyframeAreaLayout->setMargin(0);

  headers = new TimelineHeader();
  keyframeAreaLayout->addWidget(headers);

  QWidget* keyframeCenterWidget = new QWidget();

  QHBoxLayout* keyframeCenterLayout = new QHBoxLayout(keyframeCenterWidget);
  keyframeCenterLayout->setSpacing(0);
  keyframeCenterLayout->setMargin(0);

  keyframeView = new KeyframeView();

  keyframeCenterLayout->addWidget(keyframeView);

  verticalScrollBar = new QScrollBar();
  verticalScrollBar->setOrientation(Qt::Vertical);

  keyframeCenterLayout->addWidget(verticalScrollBar);


  keyframeAreaLayout->addWidget(keyframeCenterWidget);

  horizontalScrollBar = new ResizableScrollBar();
  horizontalScrollBar->setOrientation(Qt::Horizontal);

  keyframeAreaLayout->addWidget(horizontalScrollBar);

  splitter->addWidget(keyframeArea);

  hlayout->addWidget(splitter);

  setWidget(contents);
}

void EffectControls::Retranslate() {
  panel_name = tr("Effects: ");

  btnAddVideoEffect->setToolTip(tr("Add Video Effect"));
  lblVideoEffects->setText(tr("VIDEO EFFECTS"));
  btnAddVideoTransition->setToolTip(tr("Add Video Transition"));
  btnAddAudioEffect->setToolTip(tr("Add Audio Effect"));
  lblAudioEffects->setText(tr("AUDIO EFFECTS"));
  btnAddAudioTransition->setToolTip(tr("Add Audio Transition"));

  UpdateTitle();
}

void EffectControls::LoadLayoutState(const QByteArray &data)
{
  splitter->restoreState(data);
}

QByteArray EffectControls::SaveLayoutState()
{
  return splitter->saveState();
}

void EffectControls::update_scrollbar() {
  verticalScrollBar->setMaximum(qMax(0, effects_area->height() - keyframeView->height() - headers->height()));
  verticalScrollBar->setPageStep(verticalScrollBar->height());
}

void EffectControls::queue_post_update() {
  keyframeView->update();
  update_scrollbar();
}

void EffectControls::effects_area_context_menu() {
  QMenu menu(this);

  olive::MenuHelper.create_effect_paste_action(&menu);

  menu.exec(QCursor::pos());
}

void EffectControls::delete_effects() {
  // load in new clips
  if (mode_ == kTransitionNone) {

    ComboAction* ca = new ComboAction();

    for (int i=0;i<open_effects_.size();i++) {
      if (open_effects_.at(i)->IsSelected()) {
        ca->append(new EffectDeleteCommand(open_effects_.at(i)->GetEffect()));
      }
    }

    if (ca->hasActions()) {
      olive::UndoStack.push(ca);
      panel_sequence_viewer->viewer_widget->frame_update();
    } else {
      delete ca;
    }
  }
}

void EffectControls::Reload() {
  Clear(false);
  Load();
}

void EffectControls::SetClips(const QVector<Clip*> &clips, int mode)
{
  Clear(true);

  // replace clip vector
  selected_clips_ = clips;
  mode_ = mode;

  Load();
}

void EffectControls::Load() {
  // load in new clips
  for (int i=0;i<selected_clips_.size();i++) {
    Clip* c = selected_clips_.at(i);

    QVBoxLayout* layout;

    if (c->track() < 0) {
      vcontainer->setVisible(true);
      layout = video_effect_layout;
    } else {
      acontainer->setVisible(true);
      layout = audio_effect_layout;
    }

    if (mode_ == kTransitionNone) {
      for (int j=0;j<c->effects.size();j++) {
        open_effect(layout, c->effects.at(j).get());
      }
    } else if (mode_ == kTransitionOpening && c->opening_transition != nullptr) {
      open_effect(layout, c->opening_transition.get());
    } else if (mode_ == kTransitionClosing && c->closing_transition != nullptr) {
      open_effect(layout, c->closing_transition.get());
    }
  }
  if (selected_clips_.size() > 0) {
    keyframeView->setEnabled(true);
    headers->setVisible(true);

    QTimer::singleShot(50, this, SLOT(queue_post_update()));
  }

  UpdateTitle();
}

void EffectControls::video_effect_click() {
  show_effect_menu(EFFECT_TYPE_EFFECT, EFFECT_TYPE_VIDEO);
}

void EffectControls::audio_effect_click() {
  show_effect_menu(EFFECT_TYPE_EFFECT, EFFECT_TYPE_AUDIO);
}

void EffectControls::video_transition_click() {
  show_effect_menu(EFFECT_TYPE_TRANSITION, EFFECT_TYPE_VIDEO);
}

void EffectControls::audio_transition_click() {
  show_effect_menu(EFFECT_TYPE_TRANSITION, EFFECT_TYPE_AUDIO);
}

void EffectControls::resizeEvent(QResizeEvent*) {
  update_scrollbar();
}

bool EffectControls::is_focused() {
  if (this->hasFocus()) return true;

  for (int i=0;i<open_effects_.size();i++) {
    if (open_effects_.at(i)->IsFocused()) {
      return true;
    }
  }

  return false;
}

EffectsArea::EffectsArea(QWidget* parent) :
  QWidget(parent)
{}

void EffectsArea::receive_wheel_event(QWheelEvent *e) {
  QApplication::sendEvent(this, e);
}
