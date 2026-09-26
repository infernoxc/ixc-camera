#pragma once

// COM entry object (the registered CLSID). Frame Server creates it, fills in the virtual
// camera's attributes (including the physical camera it associates with IXC Camera), then
// calls ActivateObject() to get the media source. Nothing heavy happens until then.

#include "virtual_camera/vcam_ids.h"

#include <mfidl.h>
#include <wrl/implements.h>

namespace ixc::vcam {

class __declspec(uuid("3011A045-BC7A-469D-86D0-2800938E32BF")) Activate final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    HRESULT RuntimeClassInitialize();

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    // IMFAttributes (delegated to an internal store)
    STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* v) override { return attrs_->GetItem(key, v); }
    STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override { return attrs_->GetItemType(key, type); }
    STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT v, BOOL* result) override { return attrs_->CompareItem(key, v, result); }
    STDMETHODIMP Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE type, BOOL* result) override { return attrs_->Compare(theirs, type, result); }
    STDMETHODIMP GetUINT32(REFGUID key, UINT32* v) override { return attrs_->GetUINT32(key, v); }
    STDMETHODIMP GetUINT64(REFGUID key, UINT64* v) override { return attrs_->GetUINT64(key, v); }
    STDMETHODIMP GetDouble(REFGUID key, double* v) override { return attrs_->GetDouble(key, v); }
    STDMETHODIMP GetGUID(REFGUID key, GUID* v) override { return attrs_->GetGUID(key, v); }
    STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override { return attrs_->GetStringLength(key, length); }
    STDMETHODIMP GetString(REFGUID key, LPWSTR v, UINT32 size, UINT32* length) override { return attrs_->GetString(key, v, size, length); }
    STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* v, UINT32* length) override { return attrs_->GetAllocatedString(key, v, length); }
    STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override { return attrs_->GetBlobSize(key, size); }
    STDMETHODIMP GetBlob(REFGUID key, UINT8* buf, UINT32 size, UINT32* written) override { return attrs_->GetBlob(key, buf, size, written); }
    STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buf, UINT32* size) override { return attrs_->GetAllocatedBlob(key, buf, size); }
    STDMETHODIMP GetUnknown(REFGUID key, REFIID riid, LPVOID* ppv) override { return attrs_->GetUnknown(key, riid, ppv); }
    STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT v) override { return attrs_->SetItem(key, v); }
    STDMETHODIMP DeleteItem(REFGUID key) override { return attrs_->DeleteItem(key); }
    STDMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID key, UINT32 v) override { return attrs_->SetUINT32(key, v); }
    STDMETHODIMP SetUINT64(REFGUID key, UINT64 v) override { return attrs_->SetUINT64(key, v); }
    STDMETHODIMP SetDouble(REFGUID key, double v) override { return attrs_->SetDouble(key, v); }
    STDMETHODIMP SetGUID(REFGUID key, REFGUID v) override { return attrs_->SetGUID(key, v); }
    STDMETHODIMP SetString(REFGUID key, LPCWSTR v) override { return attrs_->SetString(key, v); }
    STDMETHODIMP SetBlob(REFGUID key, const UINT8* buf, UINT32 size) override { return attrs_->SetBlob(key, buf, size); }
    STDMETHODIMP SetUnknown(REFGUID key, IUnknown* v) override { return attrs_->SetUnknown(key, v); }
    STDMETHODIMP LockStore() override { return attrs_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* count) override { return attrs_->GetCount(count); }
    STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* v) override { return attrs_->GetItemByIndex(index, key, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* dest) override { return attrs_->CopyAllItems(dest); }

private:
    HRESULT GetPhysicalSource(Microsoft::WRL::ComPtr<IMFMediaSource>& source);

    Microsoft::WRL::ComPtr<IMFAttributes> attrs_;
    Microsoft::WRL::ComPtr<IMFMediaSource> source_;
};

}  // namespace ixc::vcam
