#include "storage.hpp"

#include "../base/logging.hpp"
#include "../base/string_utils.hpp"

#include "../indexer/data_factory.hpp"

#include "../platform/platform.hpp"

#include "../coding/file_writer.hpp"
#include "../coding/file_reader.hpp"
#include "../coding/file_container.hpp"
#include "../coding/url_encode.hpp"

#include "../version/version.hpp"

#include "../std/algorithm.hpp"
#include "../std/target_os.hpp"
#include "../std/bind.hpp"

namespace storage
{
  const int TIndex::INVALID = -1;

//  static string ErrorString(DownloadResultT res)
//  {
//    switch (res)
//    {
//    case EHttpDownloadCantCreateFile:
//      return "File can't be created. Probably, you have no disk space available or "
//                         "using read-only file system.";
//    case EHttpDownloadFailed:
//      return "Download failed due to missing or poor connection. "
//                         "Please, try again later.";
//    case EHttpDownloadFileIsLocked:
//      return "Download can't be finished because file is locked. "
//                         "Please, try again after restarting application.";
//    case EHttpDownloadFileNotFound:
//      return "Requested file is absent on the server.";
//    case EHttpDownloadNoConnectionAvailable:
//      return "No network connection is available.";
//    case EHttpDownloadOk:
//      return "Download finished successfully.";
//    }
//    return "Unknown error";
//  }

  ////////////////////////////////////////////////////////////////////////////
  void Storage::Init(TAddMapFunction addFunc, TRemoveMapFunction removeFunc, TUpdateRectFunction updateRectFunc)
  {
    m_currentVersion = static_cast<uint32_t>(Version::BUILD);

    m_addMap = addFunc;
    m_removeMap = removeFunc;
    m_updateRect = updateRectFunc;
  }

  string Storage::UpdateBaseUrl() const
  {
    return "http://svobodu404popugajam.mapswithme.com:34568/maps/" OMIM_OS_NAME "/" + strings::to_string(m_currentVersion) + "/";
  }

  CountriesContainerT const & NodeFromIndex(CountriesContainerT const & root, TIndex const & index)
  {
    // complex logic to avoid [] out_of_bounds exceptions
    if (index.m_group == TIndex::INVALID || index.m_group >= static_cast<int>(root.SiblingsCount()))
      return root;
    else
    {
      if (index.m_country == TIndex::INVALID || index.m_country >= static_cast<int>(root[index.m_group].SiblingsCount()))
        return root[index.m_group];
      if (index.m_region == TIndex::INVALID || index.m_region >= static_cast<int>(root[index.m_group][index.m_country].SiblingsCount()))
        return root[index.m_group][index.m_country];
      return root[index.m_group][index.m_country][index.m_region];
    }
  }

  Country const & Storage::CountryByIndex(TIndex const & index) const
  {
    return NodeFromIndex(m_countries, index).Value();
  }

  size_t Storage::CountriesCount(TIndex const & index) const
  {
    return NodeFromIndex(m_countries, index).SiblingsCount();
  }

  string const & Storage::CountryName(TIndex const & index) const
  {
    return NodeFromIndex(m_countries, index).Value().Name();
  }

  string const & Storage::CountryFlag(TIndex const & index) const
  {
    return NodeFromIndex(m_countries, index).Value().Flag();
  }

  LocalAndRemoteSizeT Storage::CountrySizeInBytes(TIndex const & index) const
  {
    return CountryByIndex(index).Size();
  }

  TStatus Storage::CountryStatus(TIndex const & index) const
  {
    // first, check if we already downloading this country or have in in the queue
    TQueue::const_iterator found = std::find(m_queue.begin(), m_queue.end(), index);
    if (found != m_queue.end())
    {
      if (found == m_queue.begin())
        return EDownloading;
      else
        return EInQueue;
    }

    // second, check if this country has failed while downloading
    if (m_failedCountries.find(index) != m_failedCountries.end())
      return EDownloadFailed;

    LocalAndRemoteSizeT size = CountryByIndex(index).Size();
    if (size.first == size.second)
    {
      if (size.second == 0)
        return EUnknown;
      else
        return EOnDisk;
    }

    return ENotDownloaded;
  }

  void Storage::DownloadCountry(TIndex const & index)
  {
    // check if we already downloading this country
    TQueue::const_iterator found = find(m_queue.begin(), m_queue.end(), index);
    if (found != m_queue.end())
    { // do nothing
      return;
    }
    // remove it from failed list
    m_failedCountries.erase(index);
    // add it into the queue
    m_queue.push_back(index);
    // and start download if necessary
    if (m_queue.size() == 1)
    {
      // reset total country's download progress
      LocalAndRemoteSizeT const size = CountryByIndex(index).Size();
      m_countryProgress.first = 0;
      m_countryProgress.second = size.second;

      DownloadNextCountryFromQueue();
    }
    else
    { // notify about "In Queue" status
      if (m_observerChange)
        m_observerChange(index);
    }
  }

  template <class TRemoveFn>
  class DeactivateMap
  {
    string m_workingDir;
    TRemoveFn & m_removeFn;
  public:
    DeactivateMap(TRemoveFn & removeFn) : m_removeFn(removeFn)
    {
      m_workingDir = GetPlatform().WritableDir();
    }
    void operator()(CountryFile const & file)
    {
      m_removeFn(file.GetFileWithExt());
    }
  };

  void Storage::DownloadNextCountryFromQueue()
  {
    while (!m_queue.empty())
    {
      TIndex index = m_queue.front();
      FilesContainerT const & tiles = CountryByIndex(index).Files();
      for (FilesContainerT::const_iterator it = tiles.begin(); it != tiles.end(); ++it)
      {
        if (!IsFileDownloaded(*it))
        {
          vector<string> urls;
          urls.push_back(UpdateBaseUrl() + UrlEncode(it->GetFileWithExt()));
          m_request.reset(downloader::HttpRequest::GetFile(urls,
                                           GetPlatform().WritablePathForFile(it->GetFileWithExt()),
                                           it->m_remoteSize,
                                           bind(&Storage::OnMapDownloadFinished, this, _1),
                                           bind(&Storage::OnMapDownloadProgress, this, _1)));
          // notify GUI - new status for country, "Downloading"
          if (m_observerChange)
            m_observerChange(index);
          return;
        }
      }
      // continue with next country
      m_queue.pop_front();
      // reset total country's download progress
      if (!m_queue.empty())
      {
        m_countryProgress.first = 0;
        m_countryProgress.second = CountryByIndex(m_queue.front()).Size().second;
      }
      // and notify GUI - new status for country, "OnDisk"
      if (m_observerChange)
        m_observerChange(index);
    }
  }

  class DeleteMap
  {
    string m_workingDir;
  public:
    DeleteMap()
    {
      m_workingDir = GetPlatform().WritableDir();
    }
    /// @TODO do not delete other countries cells
    void operator()(CountryFile const & file)
    {
      FileWriter::DeleteFileX(m_workingDir + file.GetFileWithExt());
    }
  };

  template <typename TRemoveFunc>
  void DeactivateAndDeleteCountry(Country const & country, TRemoveFunc removeFunc)
  {
    // deactivate from multiindex
    for_each(country.Files().begin(), country.Files().end(), DeactivateMap<TRemoveFunc>(removeFunc));
    // delete from disk
    for_each(country.Files().begin(), country.Files().end(), DeleteMap());
  }

  m2::RectD Storage::CountryBounds(TIndex const & index) const
  {
    Country const & country = CountryByIndex(index);
    return country.Bounds();
  }

  void Storage::DeleteCountry(TIndex const & index)
  {
    Country const & country = CountryByIndex(index);

    m2::RectD bounds;

    // check if we already downloading this country
    TQueue::iterator found = find(m_queue.begin(), m_queue.end(), index);
    if (found != m_queue.end())
    {
      if (found == m_queue.begin())
      { // stop download
        m_request.reset();
        // remove from the queue
        m_queue.erase(found);
        // start another download if the queue is not empty
        DownloadNextCountryFromQueue();
      }
      else
      { // remove from the queue
        m_queue.erase(found);
      }
    }
    else
    {
      // bounds are only updated if country was already activated before
      bounds = country.Bounds();
    }

    DeactivateAndDeleteCountry(country, m_removeMap);
    if (m_observerChange)
      m_observerChange(index);

    if (bounds != m2::RectD::GetEmptyRect())
      m_updateRect(bounds);
  }

  void Storage::ReInitCountries(bool forceReload)
  {
    if (forceReload)
      m_countries.Clear();

    if (m_countries.SiblingsCount() == 0)
    {
      string json;
      ReaderPtr<Reader>(GetPlatform().GetReader(COUNTRIES_FILE)).ReadAsString(json);
      m_currentVersion = LoadCountries(json, m_countries);
      if (m_currentVersion < 0)
        LOG(LERROR, ("Can't load countries file", COUNTRIES_FILE));
    }
  }

  void Storage::Subscribe(TObserverChangeCountryFunction change, TObserverProgressFunction progress)
  {
    m_observerChange = change;
    m_observerProgress = progress;

    ReInitCountries(false);
  }

  void Storage::Unsubscribe()
  {
    m_observerChange.clear();
    m_observerProgress.clear();
  }

  void Storage::OnMapDownloadFinished(downloader::HttpRequest & request)
  {
    if (m_queue.empty())
    {
      ASSERT(false, ("Invalid url?", request.Data()));
      return;
    }

    if (request.Status() == downloader::HttpRequest::EFailed)
    {
      // remove failed country from the queue
      TIndex const failedIndex = m_queue.front();
      m_queue.pop_front();
      m_failedCountries.insert(failedIndex);
      // notify GUI about failed country
      if (m_observerChange)
        m_observerChange(failedIndex);
    }
    else
    {
      LocalAndRemoteSizeT const size = CountryByIndex(m_queue.front()).Size();
      if (size.second != 0)
        m_countryProgress.first = size.first;

      // get file descriptor
      string file = request.Data();

      // FIXME
      string::size_type const i = file.find_last_of("/\\");
      if (i != string::npos)
        file = file.substr(i+1);

      // activate downloaded map piece
      m_addMap(file);

      // update rect from downloaded file
      feature::DataHeader header;
      LoadMapHeader(GetPlatform().GetReader(file), header);
      m_updateRect(header.GetBounds());
    }
    m_request.reset();
    DownloadNextCountryFromQueue();
  }

  void Storage::OnMapDownloadProgress(downloader::HttpRequest & request)
  {
    if (m_queue.empty())
    {
      ASSERT(false, ("queue can't be empty"));
      return;
    }

    if (m_observerProgress)
    {
      downloader::HttpRequest::ProgressT p = request.Progress();
      p.first += m_countryProgress.first;
      p.second = m_countryProgress.second;
      m_observerProgress(m_queue.front(), p);
    }
  }
}
