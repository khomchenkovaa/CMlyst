/***************************************************************************
 *   Copyright (C) 2014-2015 Daniel Nicoletti <dantti12@gmail.com>         *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; see the file COPYING. If not, write to       *
 *   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,  *
 *   Boston, MA 02110-1301, USA.                                           *
 ***************************************************************************/

#include "root.h"

#include <Cutelyst/Application>
#include <Cutelyst/Context>
#include <Cutelyst/Plugins/Authentication/authentication.h>
#include <Cutelyst/Plugins/View/Grantlee/grantleeview.h>
#include <Cutelyst/Plugins/Utils/Sql>

#include <grantlee/safestring.h>

#include <QSqlQuery>

#include <QBuffer>
#include <QDebug>

#include "libCMS/page.h"
#include "libCMS/menu.h"

#include "rsswriter.h"

Root::Root(Application *app) : Controller(app)
{
}

Root::~Root()
{
}

void Root::notFound(Context *c)
{
    c->stash({
                 {QStringLiteral("template"), QStringLiteral("404.html")},
                 {QStringLiteral("cms"), QVariant::fromValue(engine)},
             });
    c->res()->setStatus(404);
}

bool Root::End(Context *c)
{
    Q_UNUSED(c)
    //    qDebug() << "*** Root::End()" << c->view();

    const QString theme = engine->settingsValue(QStringLiteral("theme"), QStringLiteral("default"));
    // Check if the theme changed
    if (m_theme != theme) {
        m_theme = theme;

        GrantleeView *view = qobject_cast<GrantleeView*>(c->app()->view());
        view->setIncludePaths({ m_themeDir.absoluteFilePath(theme) });
    }

    QString staticTheme = QLatin1String("/static/themes/") + theme;
    c->setStash(QStringLiteral("basetheme"), c->uriFor(staticTheme).toString());

    return true;
}

bool Root::postFork(Application *app)
{
    m_themeDir = app->pathTo({ QStringLiteral("root"), QStringLiteral("themes") });

    return true;
}

void Root::page(Cutelyst::Context *c)
{
    Response *res = c->res();
    Request *req = c->req();

    // Get the desired page (dispatcher already found it)
    auto page = c->stash(QStringLiteral("page")).value<CMS::Page *>();
//    QVariantHash page = c->stash(QStringLiteral("page")).toHash();

    // See if the page has changed, if the settings have changed
//     and have a newer date use that instead
    QDateTime currentDateTime = qMax(page->modified(), engine->lastModified());
//    QDateTime currentDateTime = qMax(page.value(QStringLiteral("modified")).toDateTime(), engine->lastModified());
    const QDateTime &clientDate = req->headers().ifModifiedSinceDateTime();
    if (clientDate.isValid() && currentDateTime == clientDate) {
        res->setStatus(Response::NotModified);
        return;
    }
    res->headers().setLastModified(currentDateTime);

    QString cmsPagePath = QLatin1Char('/') + c->req()->path();
    engine->setProperty("pagePath", cmsPagePath);

    auto settings = engine->settings();
    const QString cms_head = settings.value(QStringLiteral("cms_head"));
    if (!cms_head.isEmpty()) {
        const Grantlee::SafeString safe(cms_head, true);
        c->setStash(QStringLiteral("cms_head"), QVariant::fromValue(safe));
    }

    const QString cms_foot = settings.value(QStringLiteral("cms_head"));
    if (!cms_foot.isEmpty()) {
        const Grantlee::SafeString safe(cms_foot, true);
        c->setStash(QStringLiteral("cms_foot"), QVariant::fromValue(safe));
    }

    if (page->blog())  {
        c->setStash(QStringLiteral("template"), QStringLiteral("blog.html"));
    } else {
        c->setStash(QStringLiteral("template"), QStringLiteral("page.html"));
    }
    c->setStash(QStringLiteral("meta_title"), page->name());
    c->setStash(QStringLiteral("cms"), QVariant::fromValue(engine));
}

void Root::lastPosts(Context *c)
{
    Response *res = c->res();
    Request *req = c->req();
    QList<CMS::Page *> posts;
    posts = engine->listPages(c,
                              CMS::Engine::Posts,
                              CMS::Engine::SortFlags(
                                  CMS::Engine::Name |
                                  CMS::Engine::Date |
                                  CMS::Engine::Reversed),
                              -1,
                              10);

    if (!posts.isEmpty()) {
        // See if the page has changed, if the settings have changed
        // and have a newer date use that instead
        const QDateTime &currentDateTime = posts.first()->created();
        const QDateTime &clientDate = req->headers().ifModifiedSinceDateTime();
        if (clientDate.isValid()) {
            if (currentDateTime == clientDate && currentDateTime.isValid()) {
                res->setStatus(Response::NotModified);
                return;
            }
        }
        res->headers().setLastModified(currentDateTime);
    }

    QString cmsPagePath = QLatin1Char('/') + c->req()->path();
    engine->setProperty("pagePath", cmsPagePath);
    c->stash({
                 {QStringLiteral("template"), QStringLiteral("posts.html")},
                 {QStringLiteral("meta_title"), engine->settingsValue(QStringLiteral("title"))},
                 {QStringLiteral("meta_description"), engine->settingsValue(QStringLiteral("tagline"))},
                 {QStringLiteral("cms"), QVariant::fromValue(engine)},
                 {QStringLiteral("posts"), QVariant::fromValue(posts)}
             });
}

void Root::feed(Context *c)
{
    Request *req = c->req();
    Response *res = c->res();
    Headers &headers = res->headers();

    // See if the page has changed, if the settings have changed
    // and have a newer date use that instead
    const QDateTime currentDateTime = engine->lastModified();
    const QDateTime clientDate = headers.ifModifiedSinceDateTime();
    if (clientDate.isValid() && currentDateTime.isValid() && currentDateTime == clientDate) {
        res->setStatus(Response::NotModified);
        return;
    }

    headers.setLastModified(currentDateTime);
    headers.setContentType(QStringLiteral("text/xml; charset=UTF-8"));

    QSqlQuery query = CPreparedSqlQueryThreadForDB(
                QStringLiteral("SELECT name, path, author, created, content "
                               "FROM pages "
                               "WHERE blog = 1 "
                               "ORDER BY created DESC "
                               "LIMIT :limit "
                               ),
                QStringLiteral("cmlyst"));
    query.bindValue(QStringLiteral(":limit"), 10);

    auto settings = engine->settings();

    auto buffer = new QBuffer(c);
    buffer->open(QIODevice::ReadWrite);
    res->setBody(buffer);

    RSSWriter writer(buffer);

    writer.startRSS();
    writer.writeStartChannel();
    writer.writeChannelTitle(settings.value(QStringLiteral("title")));
    writer.writeChannelFeedLink(c->uriFor(c->action()).toString());
    writer.writeChannelLink(req->base());
    writer.writeChannelDescription(settings.value(QStringLiteral("tagline")));

    if (Q_LIKELY(query.exec())) {
        writer.writeChannelLastBuildDate(currentDateTime);

        while (query.next()) {
            writer.writeStartItem();

            writer.writeItemTitle(query.value(0).toString());

            const QString link = c->uriFor(query.value(1).toString()).toString();
            writer.writeItemLink(link);
            writer.writeItemCommentsLink(link + QLatin1String("#comments"));
            writer.writeItemCreator(query.value(2).toString());
            writer.writeItemPubDate(QDateTime::fromMSecsSinceEpoch(query.value(3).toLongLong() * 1000));

            const QString content = query.value(4).toString();
            writer.writeItemDescription(content.left(300));
            writer.writeItemContent(content);

            writer.writeEndItem();
        }

        writer.writeEndChannel();
    }
    writer.endRSS();
}
