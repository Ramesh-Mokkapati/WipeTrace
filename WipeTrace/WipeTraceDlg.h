// WipeTraceDlg.h : main dialog
#pragma once

#include "WipeEngine.h"
#include "FreeSpaceWiper.h"
#include "DriveInfo.h"

class CWipeTraceDlg : public CDialogEx
{
public:
    explicit CWipeTraceDlg(const std::vector<CString>& initialPaths, CWnd* pParent = nullptr);
    enum { IDD = IDD_WIPETRACE_DIALOG };

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override {}     // Enter must not start anything
    void OnCancel() override;

    afx_msg void OnAddFiles();
    afx_msg void OnAddFolder();
    afx_msg void OnRemove();
    afx_msg void OnClear();
    afx_msg void OnWipe();
    afx_msg void OnStop();
    afx_msg void OnShellToggle();
    afx_msg void OnDropFiles(HDROP hDrop);
    afx_msg BOOL OnCopyData(CWnd* pWnd, COPYDATASTRUCT* pCopyDataStruct);
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnDestroy();
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
    afx_msg void OnListKeyDown(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg LRESULT OnFirstRun(WPARAM, LPARAM);
    afx_msg LRESULT OnPlanDone(WPARAM, LPARAM);
    afx_msg LRESULT OnWipeDone(WPARAM, LPARAM);
    afx_msg LRESULT OnFreeDone(WPARAM, LPARAM);
    afx_msg void OnDriveChange();
    afx_msg void OnWipeFree();
    afx_msg void OnAddSelected();
    afx_msg void OnTreeExpanding(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnTreeDblClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnTreeKeyDown(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
    afx_msg BOOL OnDeviceChange(UINT nEventType, DWORD_PTR dwData);
    DECLARE_MESSAGE_MAP()

private:
    struct Target
    {
        CStringW path;
        bool     isDir;
        ULONGLONG size;
    };
    struct Anchor
    {
        UINT  id;
        CRect rc;
        int   flags;
    };
    struct TreeNode
    {
        CStringW  path;
        bool      isDir;
        bool      populated;
        HTREEITEM item = nullptr;
    };
    enum { ID_TREE_WIPE = 40001, ID_TREE_ADD, ID_TREE_SHOW, ID_TREE_FREESPACE };
    enum class RunKind { None, Items, FreeSpace };

    // drives and browse tree
    void RefreshDrives();
    void ResetTree();
    void PopulateNode(int node);
    HTREEITEM InsertTreeItem(HTREEITEM parent, const CStringW& label, int node, int image, bool hasKids);
    void RefreshTreeFolderOf(const CStringW& path);
    int  SelectedNode() const;
    void ShowTreeMenu(CPoint screenPt, bool fromKeyboard);
    int  RowFor(const CStringW& path) const;
    void StartWipe(const std::vector<CStringW>& paths);

    void AddPaths(const std::vector<CString>& paths);
    void AddRow(const Target& t);
    void SetRowStatus(int row, const CString& status);
    void UpdateShellState();
    void UpdateCounts();
    void SetBusy(bool busy, const CString& status);
    void SetStatus(const CString& s) { SetDlgItemText(IDC_STATUS, s); }
    WipeOptions ReadOptions() const;

    std::vector<CString> m_initialPaths;
    std::vector<Target>  m_targets;
    CListCtrl     m_list;
    CTreeCtrl     m_tree;
    CComboBox     m_drives;
    CComboBox     m_passes;
    std::vector<VolumeInfo> m_volumes;
    std::vector<TreeNode>   m_nodes;    // tree item lParam -> node
    RunKind       m_runKind = RunKind::None;
    FreeSpaceProgress m_freeProgress;
    CStringW      m_freeRoot;
    bool          m_freeCleanEntries = true;
    CSpinButtonCtrl m_spin;
    CProgressCtrl m_progress;
    HICON         m_hIcon = nullptr;

    WipeEngine    m_engine;
    WipePlan      m_plan;
    WipeOptions   m_runOptions;
    std::vector<CStringW> m_runPaths;   // targets of the current run, in m_plan's root order
    std::atomic<bool> m_cancel{ false };
    bool m_busy = false;
    bool m_closePending = false;

    std::vector<Anchor> m_anchors;
    CRect m_initClient, m_initWindow;
};
