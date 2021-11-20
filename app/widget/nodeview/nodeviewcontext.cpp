#include "nodeviewcontext.h"

#include <QBrush>
#include <QCoreApplication>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QPen>
#include <QStyleOptionGraphicsItem>

#include "core.h"
#include "node/block/block.h"
#include "node/graph.h"
#include "node/output/track/track.h"
#include "node/project/sequence/sequence.h"
#include "nodeviewitem.h"
#include "ui/colorcoding.h"

namespace olive {

#define super QGraphicsRectItem

NodeViewContext::NodeViewContext(Node *context, QGraphicsItem *item) :
  super(item),
  context_(context)
{
  if (Block *block = dynamic_cast<Block*>(context_)) {
    rational timebase = block->track()->sequence()->GetVideoParams().frame_rate_as_time_base();
    lbl_ = QCoreApplication::translate("NodeViewContext",
                                       "%1 [%2] :: %3 - %4").arg(block->GetLabelAndName(),
                                                                 Track::Reference::TypeToTranslatedString(block->track()->type()),
                                                                 Timecode::time_to_timecode(block->in(), timebase, Core::instance()->GetTimecodeDisplay()),
                                                                 Timecode::time_to_timecode(block->out(), timebase, Core::instance()->GetTimecodeDisplay()));
  } else {
    lbl_ = context_->GetLabelAndName();
  }

  const Node::PositionMap &map = context_->GetContextPositions();
  for (auto it=map.cbegin(); it!=map.cend(); it++) {
    AddChild(it.key());
  }

  connect(context_, &Node::NodeAddedToContext, this, &NodeViewContext::AddChild, Qt::DirectConnection);
  connect(context_, &Node::NodePositionInContextChanged, this, &NodeViewContext::SetChildPosition, Qt::DirectConnection);
  connect(context_, &Node::NodeRemovedFromContext, this, &NodeViewContext::RemoveChild, Qt::DirectConnection);
}

void NodeViewContext::AddChild(Node *node)
{
  if (!context_) {
    return;
  }

  NodeViewItem *item = new NodeViewItem(node, context_, this);
  item->SetFlowDirection(flow_dir_);

  connect(node, &Node::InputConnected, this, &NodeViewContext::ChildInputConnected);
  connect(node, &Node::InputDisconnected, this, &NodeViewContext::ChildInputDisconnected);

  item_map_.insert(node, item);

  if (node == context_) {
    item->SetLabelAsOutput(true);
  }

  for (auto it=node->output_connections().cbegin(); it!=node->output_connections().cend(); it++) {
    if (!it->second.IsHidden()) {
      if (NodeViewItem *other_item = item_map_.value(it->second.node())) {
        AddEdgeInternal(node, it->second, item, other_item);
      }
    }
  }

  for (auto it=node->input_connections().cbegin(); it!=node->input_connections().cend(); it++) {
    if (!it->first.IsHidden()) {
      if (NodeViewItem *other_item = item_map_.value(it->second)) {
        AddEdgeInternal(it->second, it->first, other_item, item);
      }
    }
  }

  UpdateRect();
}

void NodeViewContext::SetChildPosition(Node *node, const QPointF &pos)
{
  item_map_.value(node)->SetNodePosition(pos);
}

void NodeViewContext::RemoveChild(Node *node)
{
  disconnect(node, &Node::InputConnected, this, &NodeViewContext::ChildInputConnected);
  disconnect(node, &Node::InputDisconnected, this, &NodeViewContext::ChildInputDisconnected);

  NodeViewItem *item = item_map_.take(node);

  // Delete edges first because the edge destructor will try to reference item (maybe that should
  // be changed...)
  QVector<NodeViewEdge*> edges_to_remove = item->edges();
  foreach (NodeViewEdge *edge, edges_to_remove) {
    ChildInputDisconnected(edge->output(), edge->input());
  }

  delete item;
}

void NodeViewContext::ChildInputConnected(Node *output, const NodeInput &input)
{
  // Add edge
  if (!input.IsHidden()) {
    if (NodeViewItem* output_item = item_map_.value(output)) {
      AddEdgeInternal(output, input, output_item, item_map_.value(input.node()));
    }
  }
}

bool NodeViewContext::ChildInputDisconnected(Node *output, const NodeInput &input)
{
  // Remove edge
  for (int i=0; i<edges_.size(); i++) {
    if (edges_.at(i)->output() == output && edges_.at(i)->input() == input) {
      delete edges_.at(i);
      edges_.removeAt(i);
      return true;
    }
  }

  return false;
}

qreal GetTextOffset(const QFontMetricsF &fm)
{
  return fm.height()/2;
}

void NodeViewContext::UpdateRect()
{
  QFont f;
  QFontMetricsF fm(f);
  qreal lbl_offset = GetTextOffset(fm);

  QRectF cbr = childrenBoundingRect();
  QRectF rect = cbr;
  int pad = NodeViewItem::DefaultItemHeight();
  rect.adjust(-pad, - lbl_offset*2 - fm.height() - pad, pad, pad);
  setRect(rect);

  last_titlebar_height_ = rect.y() + (cbr.y() - rect.y()) - pad;
}

void NodeViewContext::SetFlowDirection(NodeViewCommon::FlowDirection dir)
{
  flow_dir_ = dir;

  foreach (NodeViewItem *item, item_map_) {
    item->SetFlowDirection(dir);
  }

  foreach (NodeViewEdge *edge, edges_) {
    edge->SetFlowDirection(dir);
  }
}

void NodeViewContext::SetCurvedEdges(bool e)
{
  curved_edges_ = e;

  foreach (NodeViewEdge *edge, edges_) {
    edge->SetCurved(e);
  }
}

void NodeViewContext::DeleteSelected(NodeViewDeleteCommand *command)
{
  // Delete any selected edges
  foreach (NodeViewEdge *edge, edges_) {
    if (edge->isSelected()) {
      command->AddEdge(edge->output(), edge->input());
    }
  }

  // Delete any selected nodes
  foreach (NodeViewItem *node, item_map_) {
    if (node->isSelected()) {
      command->AddNode(node->GetNode(), context_);
    }
  }
}

void NodeViewContext::Select(const QVector<Node *> &nodes)
{
  foreach (Node *n, nodes) {
    item_map_.value(n)->setSelected(true);
  }
}

QVector<NodeViewItem *> NodeViewContext::GetSelectedItems() const
{
  QVector<NodeViewItem *> items;

  for (auto it=item_map_.cbegin(); it!=item_map_.cend(); it++) {
    if (it.value()->isSelected()) {
      items.append(it.value());
    }
  }

  return items;
}

QPointF NodeViewContext::MapScenePosToNodePosInContext(const QPointF &pos) const
{
  for (auto it=item_map_.cbegin(); it!=item_map_.cend(); it++) {
    QPointF pos_inside_parent = it.value()->mapToParent(it.value()->mapFromScene(pos));
    return NodeViewItem::ScreenToNodePoint(pos_inside_parent, flow_dir_);
  }
  return QPointF(0, 0);
}

void NodeViewContext::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
  // Set pen and brush
  Color color = context_->color();
  QColor c = color.toQColor();
  QPen pen(c, 2);
  if (option->state & QStyle::State_Selected) {
    pen.setStyle(Qt::DotLine);
  }
  painter->setPen(pen);

  QColor bg = c;
  bg.setAlpha(128);
  painter->setBrush(bg);

  // Draw semi-transparent rect for whole item
  int rounded = painter->fontMetrics().height();
  painter->drawRoundedRect(rect(), rounded, rounded);

  // Draw solid background for titlebar
  QRectF titlebar_rect = rect();
  titlebar_rect.setHeight(last_titlebar_height_ - rect().top());
  painter->setClipRect(titlebar_rect);
  painter->setBrush(c);
  painter->drawRoundedRect(rect(), rounded, rounded);
  painter->setClipping(false);

  // Draw titlebar text
  painter->setPen(ColorCoding::GetUISelectorColor(color));

  int offset = GetTextOffset(painter->fontMetrics());

  QRectF text_rect = rect();
  text_rect.adjust(offset, offset, -offset, -offset);
  painter->drawText(text_rect, lbl_);
}

QVariant NodeViewContext::itemChange(GraphicsItemChange change, const QVariant &value)
{
  return super::itemChange(change, value);
}

void NodeViewContext::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
  bool clicked_inside_titlebar = (event->pos().y() < last_titlebar_height_);

  setFlag(ItemIsMovable, clicked_inside_titlebar);
  setFlag(ItemIsSelectable, clicked_inside_titlebar);

  super::mousePressEvent(event);
}

NodeViewEdge* NodeViewContext::AddEdgeInternal(Node *output, const NodeInput& input, NodeViewItem *from, NodeViewItem *to)
{
  NodeViewEdge* edge_ui = new NodeViewEdge(output, input, from, to, this);

  edge_ui->SetFlowDirection(flow_dir_);
  edge_ui->SetCurved(curved_edges_);

  edges_.append(edge_ui);

  return edge_ui;
}

}
