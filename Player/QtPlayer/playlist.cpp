#include "playlist.h"
#include "ui_playlist.h"
#include <QMenu>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QFileInfo>

PlayList::PlayList(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PlayList)
{
    ui->setupUi(this);
}

PlayList::~PlayList()
{
    delete ui;
}

void PlayList::addFile(const QString &path)
{
    QFileInfo fi(path);
    QListWidgetItem *item = new QListWidgetItem(fi.fileName());
    item->setData(Qt::UserRole, path);      // 存完整路径
    ui->listWidget->addItem(item);
}

QListWidget *PlayList::getListWidget() const
{
    return ui->listWidget;
}

int PlayList::currentRow() const
{
    return ui->listWidget->currentRow();
}
void PlayList::setCurrentRow(int row)
{
    if (row >= 0 && row < ui->listWidget->count())
        ui->listWidget->setCurrentRow(row);
}
void PlayList::removeCurrentItem()
{
    int row = ui->listWidget->currentRow();
    if (row < 0) return;
    delete ui->listWidget->takeItem(row);
    if(ui->listWidget->count()==0){
        emit noMoreToPlay();
    }
}
int PlayList::count() const
{
    return ui->listWidget->count();
}
QString PlayList::itemPath(int row) const
{
    if (row < 0 || row >= ui->listWidget->count()) return QString();
    return ui->listWidget->item(row)->data(Qt::UserRole).toString();
}
void PlayList::next()
{
    int n = ui->listWidget->count();
    if (n == 0) return;
    int row = (ui->listWidget->currentRow() + 1) % n;   // 环形
    ui->listWidget->setCurrentRow(row);
}
void PlayList::prev()
{
    int n = ui->listWidget->count();
    if (n == 0) return;
    int row = (ui->listWidget->currentRow() - 1 + n) % n;  // 环形
    ui->listWidget->setCurrentRow(row);
}

void PlayList::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    QAction *del = menu.addAction("删除");
    QAction *play = menu.addAction("播放");
    QAction *selected = menu.exec(e->globalPos());
    if (selected == del) {
        removeCurrentItem();
    }else if (selected == play) {
        emit playThis(ui->listWidget->currentItem());
    }
}

void PlayList::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Delete) {
        removeCurrentItem();
        return;
    }
    QWidget::keyPressEvent(e);
}


