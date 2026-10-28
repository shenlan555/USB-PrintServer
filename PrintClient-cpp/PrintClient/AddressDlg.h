#pragma once

// 打印机连接方式选择窗口（对应 C# MainForm.ShowAddressSelector 里的临时 Form）
//
// 让用户选择用主机名还是 IP 连接打印机；返回 DialogResult.Yes 表示主机名。

#include <afxwin.h>
#include <string>

#include "resource.h"

namespace ps
{

class CAddressDlg : public CDialogEx
{
    DECLARE_DYNAMIC(CAddressDlg)

public:
    CAddressDlg(const std::string& hostname, const std::string& ip, CWnd* pParent = nullptr);

    enum { IDD = IDD_ADDRESS_DIALOG };

    // 对应 form.ShowDialog() == DialogResult.Yes
    bool ChoseHostname() const { return m_choseHostname; }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;

    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);

    DECLARE_MESSAGE_MAP()

private:
    std::string m_hostname;
    std::string m_ip;
    bool m_choseHostname = true;
};

} // namespace ps
