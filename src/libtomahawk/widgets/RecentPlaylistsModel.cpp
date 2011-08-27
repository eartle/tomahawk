/*
    Copyright (C) 2011  Leo Franchi <lfranchi@kde.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/


#include "RecentPlaylistsModel.h"

#include "tomahawksettings.h"
#include "audio/audioengine.h"
#include "sourcelist.h"
#include "utils/logger.h"
#include "dynamic/DynamicPlaylist.h"
#include "database/database.h"
#include "database/databasecommand_loadallsortedplaylists.h"
#include "RecentlyPlayedPlaylistsModel.h"
#include <network/servent.h>

using namespace Tomahawk;


RecentPlaylistsModel::RecentPlaylistsModel( unsigned int maxPlaylists, QObject* parent )
    : QAbstractListModel( parent )
    , m_maxPlaylists( maxPlaylists )
{

    if ( Servent::instance()->isReady() )
        onReady();
    else
        connect( Servent::instance(), SIGNAL( ready() ), this, SLOT( onReady() ) );

    // Load recent playlists initially
    refresh();
}

void
RecentPlaylistsModel::refresh()
{
    DatabaseCommand_LoadAllSortedPlaylists* cmd = new DatabaseCommand_LoadAllSortedPlaylists( source_ptr() );
    cmd->setLimit( 15 );
    cmd->setSortOrder( DatabaseCommand_LoadAllPlaylists::ModificationTime );
    cmd->setSortAscDesc( DatabaseCommand_LoadAllPlaylists::Descending );
    connect( cmd, SIGNAL( done( QList<DatabaseCommand_LoadAllSortedPlaylists::SourcePlaylistPair> ) ), this, SLOT( playlistsLoaded( QList<DatabaseCommand_LoadAllSortedPlaylists::SourcePlaylistPair> ) ) );
    Database::instance()->enqueue( QSharedPointer< DatabaseCommand >( cmd ) );
}

void
RecentPlaylistsModel::onReady()
{
    foreach( const source_ptr& s, SourceList::instance()->sources() )
        onSourceAdded( s );

    connect( SourceList::instance(), SIGNAL( sourceAdded( Tomahawk::source_ptr ) ), this, SLOT( onSourceAdded( Tomahawk::source_ptr ) ), Qt::QueuedConnection );
    refresh();
}


void
RecentPlaylistsModel::playlistsLoaded ( const QList<DatabaseCommand_LoadAllSortedPlaylists::SourcePlaylistPair>& playlistGuids )
{
    beginResetModel();
    m_playlists.clear();

    DatabaseCommand_LoadAllSortedPlaylists::SourcePlaylistPair plPair;
    foreach ( plPair, playlistGuids )
    {
        source_ptr s = SourceList::instance()->get( plPair.first );
        if ( s.isNull() )
            continue;

        if ( plPair.first == 0 )
            s = SourceList::instance()->getLocal();

        playlist_ptr pl = s->collection()->playlist( plPair.second );
        if ( pl.isNull() )
            pl = s->collection()->autoPlaylist( plPair.second );
        if ( pl.isNull() )
            pl = s->collection()->station( plPair.second );

        if ( pl.isNull() )
        {
            qDebug() << "Found a playlist that is NOT LOADED FOR ANY SOURCE:" << plPair.first << plPair.second;
            continue;
        }
        connect( pl.data(), SIGNAL( changed() ), this, SLOT( updatePlaylist() ) );
        m_playlists << pl;
    }

    endResetModel();
}


QVariant
RecentPlaylistsModel::data( const QModelIndex& index, int role ) const
{
    if( !index.isValid() || !hasIndex( index.row(), index.column(), index.parent() ) )
        return QVariant();

    playlist_ptr pl = m_playlists[index.row()];
    switch( role )
    {
    case Qt::DisplayRole:
        return pl->title();
    case RecentlyPlayedPlaylistsModel::PlaylistRole:
        return QVariant::fromValue< Tomahawk::playlist_ptr >( pl );
    case RecentlyPlayedPlaylistsModel::ArtistRole:
    {
        if( m_artists.value( pl ).isEmpty() )
        {
            QStringList artists;

            foreach( const Tomahawk::plentry_ptr& entry, pl->entries() )
            {
                if ( !artists.contains( entry->query()->artist() ) )
                    artists << entry->query()->artist();
            }

            m_artists[pl] = artists.join( ", " );
        }

        return m_artists[pl];
    }
    case RecentlyPlayedPlaylistsModel::PlaylistTypeRole:
    {
        if ( !pl.dynamicCast< Tomahawk::DynamicPlaylist >().isNull() )
        {
            dynplaylist_ptr dynp = pl.dynamicCast< Tomahawk::DynamicPlaylist >();
            if ( dynp->mode() == Static )
                return RecentlyPlayedPlaylistsModel::AutoPlaylist;
            else if ( dynp->mode() == OnDemand )
                return RecentlyPlayedPlaylistsModel::Station;
        } else
        {
            return RecentlyPlayedPlaylistsModel::StaticPlaylist;
        }
    }
    case RecentlyPlayedPlaylistsModel::DynamicPlaylistRole:
    {
        dynplaylist_ptr dynp = pl.dynamicCast< Tomahawk::DynamicPlaylist >();
        return QVariant::fromValue< Tomahawk::dynplaylist_ptr >( dynp );
    }
    case RecentlyPlayedPlaylistsModel::TrackCountRole:
    {
        if ( !pl.dynamicCast< Tomahawk::DynamicPlaylist >().isNull() && pl.dynamicCast< Tomahawk::DynamicPlaylist >()->mode() == OnDemand )
            return QString( QChar( 0x221E ) );
        else
            return pl->entries().count();
    }
    default:
        return QVariant();
    }
}

void
RecentPlaylistsModel::updatePlaylist()
{
    Playlist* p = qobject_cast< Playlist* >( sender() );
    Q_ASSERT( p );

    for ( int i = 0; i < m_playlists.size(); i++ )
    {
        if ( m_playlists[ i ]->guid() == p->guid() )
        {
            QModelIndex idx = index( i, 0, QModelIndex() );
            emit dataChanged( idx, idx );
        }
    }
}

void
RecentPlaylistsModel::onSourceAdded( const Tomahawk::source_ptr& source )
{
    connect( source.data(), SIGNAL( online() ), this, SLOT( sourceOnline() ) );
    connect( source->collection().data(), SIGNAL( playlistsAdded( QList<Tomahawk::playlist_ptr> ) ), SLOT( refresh() ), Qt::QueuedConnection );
    connect( source->collection().data(), SIGNAL( autoPlaylistsAdded(QList<Tomahawk::dynplaylist_ptr>)), SLOT( refresh() ), Qt::QueuedConnection );
    connect( source->collection().data(), SIGNAL( stationsAdded(QList<Tomahawk::dynplaylist_ptr>)), SLOT( refresh() ), Qt::QueuedConnection );
    connect( source->collection().data(), SIGNAL( playlistsDeleted( QList<Tomahawk::playlist_ptr> ) ), SLOT( onPlaylistsRemoved( QList<Tomahawk::playlist_ptr> ) ) );
    connect( source->collection().data(), SIGNAL( autoPlaylistsDeleted(QList<Tomahawk::dynplaylist_ptr>) ), SLOT( onDynPlaylistsRemoved( QList<Tomahawk::dynplaylist_ptr> ) ) );
    connect( source->collection().data(), SIGNAL( stationsDeleted(QList<Tomahawk::dynplaylist_ptr>) ), SLOT( onDynPlaylistsRemoved( QList<Tomahawk::dynplaylist_ptr> ) ) );
}

void
RecentPlaylistsModel::sourceOnline()
{
    Source* s = qobject_cast< Source* >( sender() );
    Q_ASSERT( s );

    for ( int i = 0; i < m_playlists.size(); i++ )
    {
        if ( m_playlists[ i ]->author().data() == s )
        {
            QModelIndex idx = index( i, 0, QModelIndex() );
            emit dataChanged( idx, idx );
        }
    }
}

void
RecentPlaylistsModel::onDynPlaylistsRemoved( QList< dynplaylist_ptr > playlists )
{
    QList< playlist_ptr > pls;
    foreach( const dynplaylist_ptr& p, playlists )
        pls << p;
    onPlaylistsRemoved( pls );
}


void
RecentPlaylistsModel::onPlaylistsRemoved( QList< playlist_ptr > playlists )
{
    foreach( const playlist_ptr& pl, playlists ) {
        if( m_playlists.contains( pl ) ) {
            m_artists.remove( pl );

            int idx = m_playlists.indexOf( pl );
            beginRemoveRows( QModelIndex(), idx, idx );
            m_playlists.removeAt( idx );
            endRemoveRows();
        }
    }

    emit emptinessChanged( m_playlists.isEmpty() );
}


int
RecentPlaylistsModel::rowCount( const QModelIndex& ) const
{
    return m_playlists.count();
}