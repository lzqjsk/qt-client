/*
 * This file is part of the xTuple ERP: PostBooks Edition, a free and
 * open source Enterprise Resource Planning software suite,
 * Copyright (c) 1999-2019 by OpenMFG LLC, d/b/a xTuple.
 * It is licensed to you under the Common Public Attribution License
 * version 1.0, the full text of which (including xTuple-specific Exhibits)
 * is available at www.xtuple.com/CPAL.  By using this software, you agree
 * to be bound by its terms.
 */

#include "materialReceiptTrans.h"

#include <QMessageBox>
#include <QSqlError>
#include <QValidator>
#include <QVariant>

#include "inputManager.h"
#include "distributeInventory.h"
#include "issueWoMaterialItem.h"
#include "storedProcErrorLookup.h"
#include "errorReporter.h"

materialReceiptTrans::materialReceiptTrans(QWidget* parent, const char* name, Qt::WindowFlags fl)
    : XWidget(parent, name, fl)
{
  setupUi(this);

  connect(_item,      SIGNAL(newId(int)), this, SLOT(sPopulateQty()));
  connect(_post,      SIGNAL(clicked()), this, SLOT(sPost()));
  connect(_warehouse, SIGNAL(newID(int)), this, SLOT(sPopulateQty()));
  connect(_cost,      SIGNAL(textChanged(const QString&)), this, SLOT(sCostUpdated()));

  _captive = false;

  _item->setType(ItemLineEdit::cGeneralInventory | ItemLineEdit::cActive);
  _warehouse->setType(WComboBox::AllActiveInventory);
  _qty->setValidator(omfgThis->qtyVal());
  _beforeQty->setPrecision(omfgThis->qtyVal());
  _afterQty->setPrecision(omfgThis->qtyVal());
  _unitCost->setPrecision(omfgThis->costVal());
  _cost->setValidator(omfgThis->costVal());
  _wo->setType(cWoOpen | cWoExploded | cWoReleased | cWoIssued);

  omfgThis->inputManager()->notify(cBCWorkOrder, this, _wo, SLOT(setId(int)));
  omfgThis->inputManager()->notify(cBCItem, this, _item, SLOT(setItemid(int)));
  omfgThis->inputManager()->notify(cBCItemSite, this, _item, SLOT(setItemsiteid(int)));

  if (!_metrics->boolean("MultiWhs"))
  {
    _warehouseLit->hide();
    _warehouse->hide();
  }

  if (!_metrics->boolean("AllowAvgCostMethod"))
    _tab->removeTab(0);

  _item->setFocus();

  _itemsiteId = 0;
  _controlledItem = false;
}

materialReceiptTrans::~materialReceiptTrans()
{
  // no need to delete child widgets, Qt does it all for us
}

void materialReceiptTrans::languageChange()
{
  retranslateUi(this);
}

enum SetResponse materialReceiptTrans::set(const ParameterList &pParams)
{
  XWidget::set(pParams);
  QVariant param;
  bool     valid;
  int      _invhistid = -1;

  param = pParams.value("invhist_id", &valid);
  if (valid)
    _invhistid = param.toInt();

  param = pParams.value("mode", &valid);
  if (valid)
  {
    if (param.toString() == "new")
    {
      _mode = cNew;

      _usernameLit->clear();
      _transDate->setEnabled(_privileges->check("AlterTransactionDates"));
      _transDate->setDate(omfgThis->dbDate());

      connect(_qty, SIGNAL(textChanged(const QString &)), this, SLOT(sUpdateQty(const QString &)));
      connect(_qty, SIGNAL(textChanged(const QString &)), this, SLOT(sCostUpdated()));
      connect(_warehouse, SIGNAL(newID(int)), this, SLOT(sPopulateQty()));
      connect(_issueToWo, SIGNAL(toggled(bool)), this, SLOT(sPopulateQty()));
    }
    else if (param.toString() == "view")
    {
      _mode = cView;

      _transDate->setEnabled(false);
      _item->setReadOnly(true);
      _warehouse->setEnabled(false);
      _qty->setEnabled(false);
      _issueToWo->setEnabled(false);
      _wo->setReadOnly(true);
      _documentNum->setEnabled(false);
      _notes->setEnabled(false);
      _post->hide();
      _close->setText(tr("&Close"));

      XSqlQuery popq;
      popq.prepare( "SELECT * "
                 "FROM invhist "
                 "WHERE (invhist_id=:invhist_id);" );
      popq.bindValue(":invhist_id", _invhistid);
      popq.exec();
      if (popq.first())
      {
        // _item first so it doesn't trigger sPopulateQty
        _item->setItemsiteid(popq.value("invhist_itemsite_id").toInt());
        _transDate->setDate(popq.value("invhist_transdate").toDate());
        _username->setText(popq.value("invhist_user").toString());
        _qty->setDouble(popq.value("invhist_invqty").toDouble());
        _beforeQty->setDouble(popq.value("invhist_qoh_before").toDouble());
        _afterQty->setDouble(popq.value("invhist_qoh_after").toDouble());
        _documentNum->setText(popq.value("invhist_ordnumber"));
        _notes->setText(popq.value("invhist_comments").toString());
      }
      else if (ErrorReporter::error(QtCriticalMsg, this, tr("Error Retrieving Item Information"),
                                    popq, __FILE__, __LINE__))
      {
        return UndefinedError;
      }
    }
  }

  return NoError;
}

void materialReceiptTrans::sPost()
{
  int itemlocSeries = 0;
  double cost = _cost->toDouble();

  struct {
    bool        condition;
    QString     msg;
    QWidget     *widget;
  } error[] = {
    { ! _item->isValid(),
      tr("You must select an Item before posting this transaction."), _item },
    { _qty->text().length() == 0 || _qty->toDouble() <= 0,
      tr("<p>You must enter a positive Quantity before posting this Transaction."),
      _qty },
    { _costAdjust->isEnabled() && _costAdjust->isChecked() && _costManual->isChecked() && (_cost->text().length() == 0 || cost ==0),
      tr("<p>You must enter a total cost value for the inventory to be transacted."),
      _cost },
    { true, "", NULL }
  };

  int errIndex;
  for (errIndex = 0; ! error[errIndex].condition; errIndex++)
    ;
  if (! error[errIndex].msg.isEmpty())
  {
    QMessageBox::critical(this, tr("Cannot Post Transaction"),
                          error[errIndex].msg);
    error[errIndex].widget->setFocus();
    return;
  }

  // Stage cleanup function to be called on error
  XSqlQuery cleanup;
  cleanup.prepare("SELECT deleteitemlocseries(:itemlocSeries, TRUE);");
  
  // Get parent series id
  XSqlQuery parentSeries;
  parentSeries.prepare("SELECT NEXTVAL('itemloc_series_seq') AS result;");
  parentSeries.exec();
  if (parentSeries.first() && parentSeries.value("result").toInt() > 0)
  {
    itemlocSeries = parentSeries.value("result").toInt();
    cleanup.bindValue(":itemlocSeries", itemlocSeries);
  }
  else
  {
    ErrorReporter::error(QtCriticalMsg, this, tr("Failed to Retrieve the Next itemloc_series_seq"),
      parentSeries, __FILE__, __LINE__);
    return;
  }

  // If controlled item: create the parent itemlocdist record, call distributeInventory::seriesAdjust
  if (_controlledItem)
  {
    XSqlQuery parentItemlocdist;
    parentItemlocdist.prepare("SELECT createitemlocdistparent(:itemsite_id, :qty, 'RX'::TEXT, NULL, :itemlocSeries) AS result;");
    parentItemlocdist.bindValue(":itemsite_id", _itemsiteId);
    parentItemlocdist.bindValue(":qty", _qty->toDouble());
    parentItemlocdist.bindValue(":itemlocSeries", itemlocSeries);
    parentItemlocdist.exec();
    if (parentItemlocdist.first())
    {
      if (distributeInventory::SeriesAdjust(itemlocSeries, this, QString(), QDate(),
        QDate(), true) == XDialog::Rejected)
      {
        cleanup.exec();
        QMessageBox::information(this, tr("Enter Receipt"),
                               tr("Transaction Canceled") );
        return;
      }
    }
    else if (ErrorReporter::error(QtCriticalMsg, this, tr("Error Creating itemlocdist Records"),
                              parentItemlocdist, __FILE__, __LINE__))
    {
      return;
    }
  }

  // Proceed to posting inventory transaction
  XSqlQuery materialPost;
  materialPost.prepare("SELECT invReceipt(:itemsiteId, :qty, '', :docNumber,"
             "                  :comments, :date, :cost, :itemlocSeries, TRUE) AS result;");
  materialPost.bindValue(":itemsiteId", _itemsiteId);
  materialPost.bindValue(":qty", _qty->toDouble());
  materialPost.bindValue(":docNumber", _documentNum->text());
  materialPost.bindValue(":comments", _notes->toPlainText());
  materialPost.bindValue(":date",        _transDate->date());
  materialPost.bindValue(":itemlocSeries", itemlocSeries);
  if(!_costAdjust->isChecked())
    materialPost.bindValue(":cost", 0.0);
  else if(_costManual->isChecked())
    materialPost.bindValue(":cost", cost);
  materialPost.exec();
  if (materialPost.first())
  {
    int result = materialPost.value("result").toInt();
    if (result < 0 || result != itemlocSeries)
    {
      cleanup.exec();
      ErrorReporter::error(QtCriticalMsg, this, tr("Error Retrieving Inventory Information"),
                             storedProcErrorLookup("invReceipt", result),
                             __FILE__, __LINE__);
      return;
    }
  } 
  else if (materialPost.lastError().type() != QSqlError::NoError)
  {
    cleanup.exec();
    ErrorReporter::error(QtCriticalMsg, this, tr("Error Retrieving Inventory Information"),
                         materialPost, __FILE__, __LINE__);
    return;
  }
  else
  {
    cleanup.exec();
    ErrorReporter::error(QtCriticalMsg, this, tr("Error Occurred"),
                         tr("%1: <p>No transaction was done because Item %2 "
                            "was not found at Site %3.")
                         .arg(windowTitle())
                         .arg(_item->itemNumber())
                         .arg(_warehouse->currentText()),__FILE__,__LINE__);
    return;
  }

  if (_issueToWo->isChecked())
  {
    materialPost.prepare( "SELECT womatl_id, womatl_issuemethod "
               "FROM womatl, wo, itemsite "
               "WHERE ( ( womatl_itemsite_id=itemsite_id)"
               " AND (womatl_wo_id=wo_id)"
               " AND (itemsite_item_id=:item_id)"
               " AND (itemsite_warehous_id=:warehous_id)"
               " AND (wo_id=:wo_id) );" );
    materialPost.bindValue(":item_id", _item->id());
    materialPost.bindValue(":warehous_id", _warehouse->id());
    materialPost.bindValue(":wo_id", _wo->id());
    materialPost.exec();
    if (materialPost.first())
    {
      if ( (materialPost.value("womatl_issuemethod").toString() == "S") ||
           (materialPost.value("womatl_issuemethod").toString() == "M") )
      {
        issueWoMaterialItem newdlg(this);
        ParameterList params;
        params.append("wo_id", _wo->id());
        params.append("womatl_id", materialPost.value("womatl_id").toInt());
        params.append("qty", _qty->toDouble());
        if (newdlg.set(params) == NoError)
          newdlg.exec();
      }
    }
  }

  if (_captive)
    close();
  else
  {
    _close->setText(tr("&Close"));
    _item->setId(-1);
    _qty->clear();
    _beforeQty->clear();
    _afterQty->clear();
    _documentNum->clear();
    _issueToWo->setChecked(false);
    _wo->setId(-1);
    _notes->clear();

    _item->setFocus();
  }
}

void materialReceiptTrans::sPopulateQty()
{
  XSqlQuery materialPopulateQty;
  materialPopulateQty.prepare( "SELECT itemsite_qtyonhand, itemsite_costmethod, itemsite_id, "
             "  isControlledItemsite(itemsite_id) AS controlled "
             "FROM itemsite "
             "WHERE ( (itemsite_item_id=:item_id)"
             " AND (itemsite_warehous_id=:warehous_id) );" );
  materialPopulateQty.bindValue(":item_id", _item->id());
  materialPopulateQty.bindValue(":warehous_id", _warehouse->id());
  materialPopulateQty.exec();
  if (materialPopulateQty.first())
  {
    _itemsiteId = materialPopulateQty.value("itemsite_id").toInt();
    _controlledItem = materialPopulateQty.value("controlled").toBool();
    _cachedQOH = materialPopulateQty.value("itemsite_qtyonhand").toDouble();
    if(_cachedQOH == 0.0)
      _costManual->setChecked(true);
    _beforeQty->setDouble(materialPopulateQty.value("itemsite_qtyonhand").toDouble());
    _costAdjust->setChecked(true);
    _costAdjust->setEnabled(materialPopulateQty.value("itemsite_costmethod").toString() == "A");

    if (_issueToWo->isChecked())
      _afterQty->setDouble(materialPopulateQty.value("itemsite_qtyonhand").toDouble());
    else if (_qty->toDouble() != 0)
      _afterQty->setDouble(_cachedQOH + _qty->toDouble());

    if (_item->isFractional())
      _qty->setValidator(omfgThis->transQtyVal());
    else
      _qty->setValidator(new QIntValidator(this));

  }
  else if (ErrorReporter::error(QtCriticalMsg, this, tr("Error Retrieving Inventory Information"),
                                materialPopulateQty, __FILE__, __LINE__))
  {
    return;
  }

  _wo->setWarehouse(_warehouse->id());
}

void materialReceiptTrans::sUpdateQty(const QString &pQty)
{
  if (_issueToWo->isChecked())
    _afterQty->setDouble(_beforeQty->toDouble());
  else
    _afterQty->setDouble(_cachedQOH + pQty.toDouble());
}

void materialReceiptTrans::sCostUpdated()
{
  if(_cost->toDouble() == 0.0 || _qty->toDouble() == 0.0)
    _unitCost->setText(tr("N/A"));
  else
    _unitCost->setDouble(_cost->toDouble() / _qty->toDouble());
}

